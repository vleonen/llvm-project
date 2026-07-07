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
// 1. ADRP+ADD (linker-relaxed GOT reference → direct address):
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
//    address — which is now wrong. We create a GOTENT symbol at the old GOT
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

      // Find the next meaningful instruction, skipping NOPs that may have
      // been inserted between ADRP and ADD/LDR (e.g., to prevent linker
      // relaxation of GOT loads). In compiler-generated code, ADRP and its
      // paired ADD/LDR are always adjacent, but hand-written assembly or
      // compiler alignment directives may insert NOPs between them.
      auto NextII = std::next(II);
      while (NextII != BB.end() && BC.MIB->isNoop(*NextII))
        ++NextII;
      if (NextII == BB.end())
        continue;

      MCInst &Next = *NextII;

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

      // Verify ADRP destination register matches LDR base register.
      if (Next.getOperand(1).isReg() &&
          Adrp.getOperand(0).getReg() == Next.getOperand(1).getReg()) {
        // Verify the LDR also references __BOLT_got_zero (operand 2).
        const MCSymbol *LdrSymbol = BC.MIB->getTargetSymbol(Next, 2);
        if (!LdrSymbol || LdrSymbol->getName() != "__BOLT_got_zero")
          continue;

        // The ADRP addend is the old GOT page address (4KB-aligned).
        // The LDR addend is the offset within that page (bits 0-11).
        // The full old GOT entry address is their sum.
        const int64_t AdrpAddend = BC.MIB->getTargetAddend(Adrp);
        const int64_t GOTEntryAddr =
            AdrpAddend + BC.MIB->getTargetAddend(Next, 2);
        if (!GOTEntryAddr)
          continue;

        auto L = BC.scopeLock();

        // Create or reuse a symbol at the old GOT entry address.
        // This symbol will be emitted as a label in the .got section by
        // emitDataSections(), enabling JITLink to resolve it to the new
        // GOT entry address.
        MCSymbol *GOTENT = BC.getOrCreateGlobalSymbol(GOTEntryAddr, "GOTENT");

        // Retarget ADRP to GOTENT (page-relative: computes the 4KB page
        // containing the GOT entry). The relocation type
        // R_AARCH64_ADR_PREL_PG_HI21 causes getTargetExprFor to produce
        // an S_ABS_PAGE expression, which MCStreamer encodes as a
        // page-relative fixup that JITLink resolves correctly.
        BC.MIB->setOperandToSymbolRef(Adrp, /*OpNum*/ 1, GOTENT, /*Addend*/ 0,
                                      BC.Ctx.get(),
                                      ELF::R_AARCH64_ADR_PREL_PG_HI21);

        // Retarget LDR to GOTENT (page offset: extracts bits 12-15 of the
        // GOT entry address, scaled by 8 for 64-bit loads). The relocation
        // type R_AARCH64_LDST64_ABS_LO12_NC causes getTargetExprFor to
        // produce an S_LO12 expression.
        BC.MIB->setOperandToSymbolRef(Next, /*OpNum*/ 2, GOTENT, /*Addend*/ 0,
                                      BC.Ctx.get(),
                                      ELF::R_AARCH64_LDST64_ABS_LO12_NC);
      }
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
