//===- bolt/Passes/ADRRelaxationPass.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the ADRRelaxationPass class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/ADRRelaxationPass.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/BinaryFormat/ELF.h"
#include <iterator>

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltCategory;

static cl::opt<bool>
    AdrPassOpt("adr-relaxation",
               cl::desc("Replace ARM non-local ADR instructions with ADRP"),
               cl::init(true), cl::cat(BoltCategory), cl::ReallyHidden);
} // namespace opts

namespace llvm {
namespace bolt {

// We don't exit directly from runOnFunction since it would call ThreadPool
// destructor which might result in internal assert if we're not finished
// creating async jobs on the moment of exit. So we're finishing all parallel
// jobs and checking the exit flag after it.
static bool PassFailed = false;

// Re-anchor an ADR + LDR pair whose loaded address crosses a section
// boundary. \p It points at the ADR computing the page-aligned address
// \p PageAddr. Scans forward (skipping NOPs, stopping at a redefinition
// of the ADR destination register) for the first load based on that
// register. When the loaded address (page + load offset) belongs to a
// different section than the page itself, retargets both the ADR and the
// LDR to a global symbol placed at the loaded address, so both are
// remapped through the section that actually contains the data.
static void reanchorCrossSectionPair(BinaryContext &BC,
                                     const BinaryBasicBlock &BB,
                                     BinaryBasicBlock::const_iterator It,
                                     const BinaryData *BD, uint64_t PageAddr) {
  MCPhysReg DestReg;
  BC.MIB->getADRReg(*It, DestReg);

  auto NextIt = std::next(It);
  while (NextIt != BB.end() && BC.MIB->isNoop(*NextIt))
    ++NextIt;
  if (NextIt == BB.end())
    return;

  const MCInst &Load = *NextIt;
  if (!BC.MIB->mayLoad(Load) || Load.getNumOperands() <= 2 ||
      !Load.getOperand(1).isReg() || Load.getOperand(1).getReg() != DestReg)
    return;

  // The load offset within the page: a symbol-annotated memory operand may
  // carry either the raw low-12 offset (analyzeRelocation GOT
  // normalization) or an absolute address (a GOTENT symbol created by
  // FixRelaxations); decode the scaled immediate otherwise.
  int64_t LoadOffset = 0;
  if (const MCSymbol *LoadSymbol = BC.MIB->getTargetSymbol(Load, 2)) {
    if (const BinaryData *LoadBD =
            BC.getBinaryDataByName(LoadSymbol->getName()))
      LoadOffset = LoadBD->getAddress() + BC.MIB->getTargetAddend(Load, 2);
    if (LoadOffset >= 0x1000)
      LoadOffset -= PageAddr;
  } else if (Load.getOperand(2).isImm()) {
    const unsigned Scale = BC.MIB->getMemScale(Load);
    if (!Scale)
      return;
    LoadOffset = Load.getOperand(2).getImm() * Scale;
  } else {
    return;
  }
  if (LoadOffset <= 0 || LoadOffset >= 0x1000)
    return;

  const uint64_t DataAddr = PageAddr + (uint64_t)LoadOffset;

  // Only re-anchor when the pair crosses into a different section: within
  // a single section the page-based remap is already correct.
  ErrorOr<BinarySection &> PageSection = BC.getSectionForAddress(PageAddr);
  ErrorOr<BinarySection &> DataSection = BC.getSectionForAddress(DataAddr);
  if (!PageSection || !DataSection || &*PageSection == &*DataSection)
    return;

  auto L = BC.scopeLock();
  MCSymbol *Anchor = BC.getOrCreateGlobalSymbol(DataAddr, "ADRPAGE");

  // ADR computes the page: point it at the data symbol so the converted
  // ADRP produces page(Anchor) and the load adds the low-12 offset.
  BC.MIB->setOperandToSymbolRef(const_cast<MCInst &>(*It), /*OpNum*/ 1, Anchor,
                                /*Addend*/ 0, BC.Ctx.get(),
                                ELF::R_AARCH64_ADR_PREL_PG_HI21);
  // LDR: resolve the unsigned-offset immediate through the same symbol.
  BC.MIB->setOperandToSymbolRef(const_cast<MCInst &>(Load), /*OpNum*/ 2, Anchor,
                                /*Addend*/ 0, BC.Ctx.get(),
                                ELF::R_AARCH64_LDST64_ABS_LO12_NC);
}

void ADRRelaxationPass::runOnFunction(BinaryFunction &BF) {
  if (PassFailed)
    return;

  BinaryContext &BC = BF.getBinaryContext();
  for (BinaryBasicBlock &BB : BF) {
    for (auto It = BB.begin(); It != BB.end(); ++It) {
      MCInst &Inst = *It;
      if (!BC.MIB->isADR(Inst))
        continue;

      const MCSymbol *Symbol = BC.MIB->getTargetSymbol(Inst);
      if (!Symbol)
        continue;

      // The linker (e.g. GNU ld with default --relax) may rewrite an ADRP
      // into an ADR while --emit-relocs keeps the original relocations;
      // such ADRs compute a page-aligned address (the page the original
      // ADRP produced). After BOLT relocates the function, the target may
      // be out of the +/-1MB ADR range, and growing the instruction is
      // impossible in non-simple functions. Converting the ADR back to
      // ADRP when its computed address (symbol address + addend) is
      // page-aligned is value-identical, needs no extra space and removes
      // the range restriction.
      if (BC.isAArch64()) {
        const int64_t Addend = BC.MIB->getTargetAddend(Inst);
        const BinaryData *BD = BC.getBinaryDataByName(Symbol->getName());
        if (BD && ((BD->getAddress() + (uint64_t)Addend) & 0xfff) == 0) {
          // GNU ld may relax an ADRP+LDR(GOT) pair into ADR+LDR. The
          // relaxed ADR computes the old page address, and the paired
          // load's offset can reach into the section that follows the
          // page's section (e.g. the page lands at the tail of
          // .data.rel.ro while the loaded word lives in .got). When
          // -rewrite moves the sections apart, remapping the ADR target
          // through the page's section produces a wrong address. Re-anchor
          // the pair to a symbol at the actually loaded address so both
          // instructions are resolved through the containing section.
          // The re-anchor retargets the pair with ADRP page/LO12 offset
          // relocation semantics, so it is only valid together with the
          // conversion below.
          reanchorCrossSectionPair(BC, BB, It, BD,
                                   BD->getAddress() + (uint64_t)Addend);
          BC.MIB->convertADRToADRP(Inst);
          continue;
        }
      }

      if (BF.hasIslandsInfo()) {
        BinaryFunction::IslandInfo &Islands = BF.getIslandInfo();
        if (Islands.Symbols.count(Symbol) || Islands.ProxySymbols.count(Symbol))
          continue;
      }

      // Don't relax adr if it points to the same function and it is not split
      // and BF initial size is < 1MB.
      const unsigned OneMB = 0x100000;
      if (!BF.isSplit() && BF.getSize() < OneMB) {
        BinaryFunction *TargetBF = BC.getFunctionForSymbol(Symbol);
        if (TargetBF && TargetBF == &BF)
          continue;
      }

      MCPhysReg Reg;
      BC.MIB->getADRReg(Inst, Reg);
      int64_t Addend = BC.MIB->getTargetAddend(Inst);
      InstructionListType Addr;

      {
        auto L = BC.scopeLock();
        Addr = BC.MIB->materializeAddress(Symbol, BC.Ctx.get(), Reg, Addend);
      }

      if (It != BB.begin() && BC.MIB->isNoop(*std::prev(It))) {
        It = BB.eraseInstruction(std::prev(It));
      } else if (std::next(It) != BB.end() && BC.MIB->isNoop(*std::next(It))) {
        BB.eraseInstruction(std::next(It));
      } else if (!opts::StrictMode && !BF.isSimple()) {
        // If the function is not simple, it may contain a jump table undetected
        // by us. This jump table may use an offset from the branch instruction
        // to land in the desired place. If we add new instructions, we
        // invalidate this offset, so we have to rely on linker-inserted NOP to
        // replace it with ADRP, and abort if it is not present.
        auto L = BC.scopeLock();
        errs() << formatv("BOLT-ERROR: Cannot relax adr in non-simple function "
                          "{0}. Use --strict option to override\n",
                          BF.getOneName());
        PassFailed = true;
        return;
      }
      It = BB.replaceInstruction(It, Addr);
    }
  }
}

void ADRRelaxationPass::runOnFunctions(BinaryContext &BC) {
  if (!opts::AdrPassOpt || !BC.HasRelocations)
    return;

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    runOnFunction(BF);
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, nullptr,
      "ADRRelaxationPass");

  if (PassFailed)
    exit(1);
}

} // end namespace bolt
} // end namespace llvm
