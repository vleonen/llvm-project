//===- bolt/Passes/FixRelaxationPass.cpp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/FixRelaxationPass.h"
#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Utils/CommandLineOpts.h"

using namespace llvm;

namespace llvm {
namespace bolt {

// This pass handles ADRP-based GOT references on AArch64 that were created
// during disassembly. The AArch64MCSymbolizer creates placeholder references
// to the synthetic symbol __BOLT_got_zero (registered at address 0) with an
// addend equal to the old GOT entry address. These placeholders need to be
// resolved to proper symbol references so that the emitter/JITLink can
// re-encode them with the correct addresses in the output binary.
//
// Two instruction patterns are handled:
//
// 1. ADRP+ADD (linker-relaxed GOT reference -> direct address):
//
//    The linker may relax an ADRP+LDR(GOT) pair into ADRP+ADD(direct address).
//    In this case, the ADD's relocation was updated during relocation reading
//    to point to the actual target symbol (e.g., a global variable). The ADRP
//    still references __BOLT_got_zero. We retarget the ADRP to the ADD's
//    symbol so the pair correctly computes the symbol's address.
//
// 2. ADRP+LDR (true GOT load, -rewrite mode only):
//
//    Both ADRP and LDR reference __BOLT_got_zero with addend = old GOT entry
//    address. In -rewrite mode, the GOT section is relocated to a new address.
//    Since __BOLT_got_zero is at address 0, JITLink would compute the OLD GOT
//    address -- which is now wrong. We create a GOTENT symbol at the old GOT
//    entry address and retarget both ADRP and LDR to reference it.
//
//    The GOTENT symbol is emitted as a label in the .got section by
//    emitDataSections(). JITLink resolves it to the new GOT entry address via
//    block layout, so the ADRP+LDR pair automatically computes the correct
//    address without any post-emission byte patching.
//
//    This mirrors handlePLTEntry() for PLT entries but is applied to regular
//    functions' non-PLT GOT references by this pass.
//
// Note: Case 2 is gated on -rewrite mode because in regular BOLT mode the GOT
// stays at its original address and __BOLT_got_zero references are already
// correct.
//
// Instruction search: the ADRP and its paired ADD/LDR may not be adjacent.
// The compiler may interleave independent instructions between them for
// scheduling. We search forward from the ADRP, looking for an ADD or LDR
// that uses the ADRP's destination register as an input. We stop if the
// register is clobbered (overwritten) before finding the paired instruction.
void FixRelaxations::runOnFunction(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();
  for (BinaryBasicBlock &BB : BF) {
    for (auto II = BB.begin(); II != BB.end(); ++II) {
      MCInst &Adrp = *II;
      if (BC.MIB->isPseudo(Adrp) || !BC.MIB->isADRP(Adrp))
        continue;

      const MCSymbol *AdrpSymbol = BC.MIB->getTargetSymbol(Adrp);
      if (!AdrpSymbol || AdrpSymbol->getName() != "__BOLT_got_zero")
        continue;

      // Search forward for the instruction paired with this ADRP.
      // The ADRP writes to operand 0 (Xd). The paired instruction (ADD or
      // LDR) uses Xd as an input. We scan forward, skipping NOPs and
      // unrelated instructions, until we find an ADD or LDR that references
      // Xd. We stop if Xd is overwritten by another instruction.
      const unsigned AdrpDestReg = Adrp.getOperand(0).getReg();
      MCInst *Paired = nullptr;

      for (auto PairII = std::next(II); PairII != BB.end(); ++PairII) {
        MCInst &Candidate = *PairII;
        if (BC.MIB->isNoop(Candidate))
          continue;
        if (BC.MIB->matchAdrpAddPair(Adrp, Candidate) ||
            (BC.MIB->mayLoad(Candidate) && Candidate.getOperand(1).isReg() &&
             Candidate.getOperand(1).getReg() == AdrpDestReg)) {
          Paired = &Candidate;
          break;
        }
        if (Candidate.getNumOperands() > 0 &&
            Candidate.getOperand(0).isReg() &&
            Candidate.getOperand(0).getReg() == AdrpDestReg &&
            !BC.MIB->isNoop(Candidate))
          break;
      }
      if (!Paired)
        continue;

      MCInst &Next = *Paired;

      // ----------------------------------------------------------------
      // Case 1: ADRP+ADD (linker-relaxed sequence)
      // ----------------------------------------------------------------
      // The linker converted the original ADRP+LDR(GOT) into ADRP+ADD(direct
      // address). The ADD's relocation now points to the real target symbol.
      // Retarget the ADRP to match the ADD's symbol so the pair computes the
      // correct (possibly relocated) target address.
      // ----------------------------------------------------------------
      if (BC.MIB->matchAdrpAddPair(Adrp, Next)) {
        const MCSymbol *Symbol = BC.MIB->getTargetSymbol(Next);
        if (!Symbol || AdrpSymbol == Symbol)
          continue;

        auto L = BC.scopeLock();
        const int64_t Addend = BC.MIB->getTargetAddend(Next);
        BC.MIB->setOperandToSymbolRef(Adrp, /*OpNum*/ 1, Symbol, Addend,
                                      BC.Ctx.get(), ELF::R_AARCH64_NONE);
        continue;
      }

      // ----------------------------------------------------------------
      // Case 2: ADRP+LDR (true GOT load, -rewrite mode only)
      // ----------------------------------------------------------------
      // Both instructions reference __BOLT_got_zero with addend = old GOT
      // entry address. In -rewrite mode the GOT moves, so we retarget both
      // to a GOTENT symbol that JITLink will resolve to the new GOT address.
      //
      // Register check: ADRP writes to Xd (operand 0), LDR reads from
      // [Xn, ...] where Xn (operand 1) must equal Xd. This confirms the
      // instructions form an address-computation pair.
      //
      // The LDR's symbolic expression is at operand 2 (the memory offset
      // for AArch64 LDRXui: operands are [Xt, Xn, offset]). We verify it
      // also targets __BOLT_got_zero to exclude unrelated loads.
      // ----------------------------------------------------------------
      if (!opts::Rewrite || !BC.MIB->mayLoad(Next))
        continue;

      if (!Next.getOperand(1).isReg() ||
          Adrp.getOperand(0).getReg() != Next.getOperand(1).getReg())
        continue;

      const MCSymbol *LdrSymbol = BC.MIB->getTargetSymbol(Next, 2);
      if (!LdrSymbol || LdrSymbol->getName() != "__BOLT_got_zero")
        continue;

      const int64_t AdrpAddend = BC.MIB->getTargetAddend(Adrp);
      const int64_t GOTEntryAddr =
          AdrpAddend + BC.MIB->getTargetAddend(Next, 2);
      if (!GOTEntryAddr)
        continue;

      auto L = BC.scopeLock();
      MCSymbol *GOTENT = BC.getOrCreateGlobalSymbol(GOTEntryAddr, "GOTENT");

      BC.MIB->setOperandToSymbolRef(Adrp, /*OpNum*/ 1, GOTENT, /*Addend*/ 0,
                                    BC.Ctx.get(),
                                    ELF::R_AARCH64_ADR_PREL_PG_HI21);
      BC.MIB->setOperandToSymbolRef(Next, /*OpNum*/ 2, GOTENT, /*Addend*/ 0,
                                    BC.Ctx.get(),
                                    ELF::R_AARCH64_LDST64_ABS_LO12_NC);
    }
  }
}

Error FixRelaxations::runOnFunctions(BinaryContext &BC) {
  if (!BC.isAArch64() || !BC.HasRelocations)
    return Error::success();

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    runOnFunction(BF);
  };

  ParallelUtilities::PredicateTy SkipFunc = [&](const BinaryFunction &BF) {
    return BF.isPLTFunction();
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_LINEAR, WorkFun,
      SkipFunc, "FixRelaxations");
  return Error::success();
}

} // namespace bolt
} // namespace llvm
