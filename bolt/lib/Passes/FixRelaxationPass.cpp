#include "bolt/Passes/FixRelaxationPass.h"
#include "bolt/Core/FunctionLayout.h"
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
//
// Base-register reuse: code may load several GOT slots through one ADRP
// (e.g. a prologue pair plus epilogue loads reloading slots from the same
// page). Only the first consumer is the paired instruction; every later
// load reading the base register with a __BOLT_got_zero operand is a reuse
// load and gets its own GOTENT, so its low-12 bits address its own slot in
// the output .got. All such slots share the ADRP's page, which requires the
// output .got to preserve the input page phase - guaranteed by the
// page-phase-preserving .got placement in mapLoadableSegmentsRewrite().
//
// Any __BOLT_got_zero reference that survives this pass would be resolved
// to address 0 by JITLink, silently encoding stale GOT offsets; runOnFunctions
// therefore fails loudly on leftovers in -rewrite mode.
void FixRelaxations::runOnFunction(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();

  // Snapshot the old GOT page(s) referenced by this function before the
  // pairing loop retargets the ADRPs: the function-wide sweep below needs
  // the page addend even after every ADRP has been re-pointed at GOTENT.
  // Also record whether any unpaired __BOLT_got_zero ADRP exists: an orphan
  // load needs a live GOT page, which in the single-page case is exactly
  // this one.
  SmallVector<int64_t, 4> GotPages;
  if (opts::Rewrite) {
    for (BinaryBasicBlock &BB : BF) {
      for (MCInst &Inst : BB) {
        if (!BC.MIB->isADRP(Inst))
          continue;
        const MCSymbol *Sym = BC.MIB->getTargetSymbol(Inst);
        if (!Sym || Sym->getName() != "__BOLT_got_zero")
          continue;
        const int64_t Page = BC.MIB->getTargetAddend(Inst);
        if (!llvm::is_contained(GotPages, Page))
          GotPages.push_back(Page);
      }
    }
  }

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
      //
      // The search continues past the end of the basic block: BOLT may split
      // the original block between the ADRP and its paired LDR (e.g. when a
      // branch targets the instruction right after the ADRP), which is
      // common in compiler-emitted code with converging address-computation
      // paths. The register-clobber stop below terminates the search at the
      // first redefinition of Xd, so ADRPs overwritten by a later ADRP
      // (redundant dead computations) are correctly left unpaired.
      const unsigned AdrpDestReg = Adrp.getOperand(0).getReg();
      MCInst *Paired = nullptr;
      // Additional GOT loads that reuse the ADRP base register after the
      // paired instruction. A single ADRP can feed multiple GOT loads
      // (e.g. a prologue pair plus epilogue loads reloading other slots
      // from the same page); each needs its own GOTENT so its low-12 bits
      // address its own slot in the output .got.
      SmallVector<MCInst *, 4> ReuseLoads;

      // Return true when the search must stop (base register redefined).
      // The first ADD/LDR reading the base register is selected as the
      // paired instruction; later loads reading the base register with a
      // __BOLT_got_zero operand are collected as reuse loads. Loads do not
      // modify the base register, so the scan continues past them; an ADD
      // pair or any other writer of the base register terminates it.
      auto scanInst = [&](MCInst &Candidate) -> bool {
        if (BC.MIB->isNoop(Candidate))
          return false;
        if (BC.MIB->matchAdrpAddPair(Adrp, Candidate)) {
          if (!Paired)
            Paired = &Candidate;
          return true; // the ADD redefines the base register
        }
        if (BC.MIB->mayLoad(Candidate) &&
            Candidate.getNumOperands() > 1 &&
            Candidate.getOperand(1).isReg() &&
            Candidate.getOperand(1).getReg() == AdrpDestReg) {
          if (!Paired) {
            Paired = &Candidate;
          } else if (const MCSymbol *S = BC.MIB->getTargetSymbol(Candidate, 2);
                     S && S->getName() == "__BOLT_got_zero") {
            ReuseLoads.push_back(&Candidate);
          }
        }
        if (Candidate.getNumOperands() > 0 &&
            Candidate.getOperand(0).isReg() &&
            Candidate.getOperand(0).getReg() == AdrpDestReg &&
            !BC.MIB->isNoop(Candidate))
          return true;
        return false;
      };

      auto scanRange = [&](auto Begin, auto End) -> bool {
        for (auto PairII = Begin; PairII != End; ++PairII)
          if (scanInst(*PairII))
            return true;
        return false;
      };
      // Scan the rest of the current block, then continue into blocks that
      // control flow reaches: the layout-next block (fall-through path of a
      // conditional branch or an unconditional branch to it), or the target
      // of a trailing unconditional branch. This is required because BOLT
      // may split the original block between the ADRP and its paired LDR
      // (e.g. when a branch targets the instruction right after the ADRP),
      // which is common in compiler-emitted code with converging
      // address-computation paths. The register-clobber stop terminates the
      // scan at the first redefinition of Xd, so ADRPs overwritten by a
      // later ADRP (redundant dead computations) are left unpaired.
      if (scanRange(std::next(II), BB.end()))
        goto PairSearchDone;
      {
        const FunctionLayout &Layout = BF.getLayout();
        SmallPtrSet<BinaryBasicBlock *, 8> Visited;
        SmallVector<BinaryBasicBlock *, 4> WorkList;
        const auto enqueue = [&](BinaryBasicBlock *Dest) {
          if (Dest && !Dest->empty() && Visited.insert(Dest).second)
            WorkList.push_back(Dest);
        };
        // Seed with the layout-next block and the unconditional branch
        // target of the ADRP block.
        {
          const unsigned NextIdx = BB.getLayoutIndex() + 1;
          if (NextIdx < Layout.block_size()) {
            auto It = Layout.blocks().begin();
            std::advance(It, NextIdx);
            enqueue(*It);
          }
          if (!BB.empty() && BC.MIB->isUnconditionalBranch(BB.back())) {
            if (BinaryBasicBlock *Taken = BB.getSuccessor(0))
              enqueue(Taken);
          }
        }
        unsigned Depth = 0;
        while (!WorkList.empty() && ++Depth < 8) {
          BinaryBasicBlock *ScanBB = WorkList.pop_back_val();
          // Skip blocks that cannot be reached from the previously scanned
          // ones: keep scanning layout-next and unconditional targets only.
          if (!BC.MIB->isUnconditionalBranch(ScanBB->back())) {
            const unsigned NextIdx = ScanBB->getLayoutIndex() + 1;
            if (NextIdx < Layout.block_size()) {
              auto It = Layout.blocks().begin();
              std::advance(It, NextIdx);
              enqueue(*It);
            }
          } else if (BinaryBasicBlock *Taken = ScanBB->getSuccessor(0)) {
            enqueue(Taken);
          }
          if (scanRange(ScanBB->begin(), ScanBB->end()))
            goto PairSearchDone;
        }
      }
    PairSearchDone:
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

      // Retarget the loads that reuse the ADRP base register. Each points
      // at its own old GOT slot (addend = slot offset within the old page,
      // the ADRP addend is the old page): give each its own GOTENT so the
      // low-12 bits address the correct slot in the output .got. The page
      // comes from the retargeted ADRP; the layout preserves the .got page
      // phase so all slots sharing an input page share an output page.
      for (MCInst *Reuse : ReuseLoads) {
        const int64_t ReuseEntryAddr =
            AdrpAddend + BC.MIB->getTargetAddend(*Reuse, 2);
        if (!ReuseEntryAddr)
          continue;
        MCSymbol *ReuseGOTENT =
            BC.getOrCreateGlobalSymbol(ReuseEntryAddr, "GOTENT");
        BC.MIB->setOperandToSymbolRef(*Reuse, /*OpNum*/ 2, ReuseGOTENT,
                                      /*Addend*/ 0, BC.Ctx.get(),
                                      ELF::R_AARCH64_LDST64_ABS_LO12_NC);
      }
    }
  }

  // Function-wide sweep for GOT loads the pairing scan could not reach.
  //
  // The scan above only traverses a bounded window (layout-order successor
  // blocks with a small depth limit) around each ADRP. GCC-style code keeps
  // the GOT base register alive across long regions (prologue to epilogue),
  // so a reuse load can sit dozens of blocks away from its ADRP and stay
  // uncollected; a stale __BOLT_got_zero operand would then be silently
  // resolved to a wrong offset. The sweep fixes any remaining non-ADRP
  // reference directly:
  //
  // A load/store's __BOLT_got_zero addend independently encodes its own
  // slot's offset within the old GOT page - the immediate is
  // self-describing. The old page is common to the whole function in the
  // unambiguous case (a single distinct page addend among the function's
  // __BOLT_got_zero ADRPs): retarget the operand to GOTENT(page + its own
  // addend), exactly like the collected reuse loads above. The page that
  // reaches the instruction at runtime comes from whichever ADRP defines
  // the base register on that path - a live GOT load's relocation proves a
  // matching GOT ADRP dominates it - and the page-phase-preserving .got
  // placement keeps page/slot consistency for every such pair.
  //
  // If the function references several distinct old GOT pages, the sweep
  // cannot tell which page an orphan's addend belongs to; leave those to
  // the runOnFunctions check, which fails loudly instead of guessing.
  // (GotPages was snapshotted before the pairing loop: by now every paired
  // ADRP references a GOTENT, not __BOLT_got_zero.)
  if (!opts::Rewrite || GotPages.size() != 1)
    return;
  const int64_t TheGotPage = GotPages.front();

  for (BinaryBasicBlock &ScanBB : BF) {
    for (MCInst &Inst : ScanBB) {
      if (BC.MIB->isADRP(Inst))
        continue;
      for (unsigned OpIdx = 0; OpIdx < Inst.getNumOperands(); ++OpIdx) {
        const MCSymbol *Sym = BC.MIB->getTargetSymbol(Inst, OpIdx);
        if (!Sym || Sym->getName() != "__BOLT_got_zero")
          continue;
        const int64_t GOTEntryAddr =
            TheGotPage + BC.MIB->getTargetAddend(Inst, OpIdx);
        if (!GOTEntryAddr)
          continue;
        auto L = BC.scopeLock();
        MCSymbol *GOTENT = BC.getOrCreateGlobalSymbol(GOTEntryAddr, "GOTENT");
        BC.MIB->setOperandToSymbolRef(Inst, OpIdx, GOTENT, /*Addend*/ 0,
                                      BC.Ctx.get(),
                                      ELF::R_AARCH64_LDST64_ABS_LO12_NC);
      }
    }
  }
}

void FixRelaxations::runOnFunctions(BinaryContext &BC) {
  if (!BC.isAArch64() || !BC.HasRelocations)
    return;

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    runOnFunction(BF);
  };

  ParallelUtilities::PredicateTy SkipFunc = [&](const BinaryFunction &BF) {
    return BF.isPLTFunction();
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_LINEAR, WorkFun,
      SkipFunc, "FixRelaxations");

  if (!opts::Rewrite)
    return;

  // Safety net: a surviving __BOLT_got_zero reference is resolved to address
  // 0 by JITLink, so a load/store would silently encode a stale GOT offset
  // (wrong-address access at runtime). This happens for shapes the pairing
  // above cannot see, e.g. a GOT access whose base register is defined by
  // something other than a __BOLT_got_zero ADRP. Fail loudly instead of
  // producing a corrupted binary. Only the offset operands (loads/stores)
  // are fatal: an unpaired ADRP computes a page that, if the ADRP is dead
  // (redefined before use, e.g. redundant compiler-emitted computations),
  // is never dereferenced - and a live unpaired ADRP implies its consumer
  // load/store is flagged here as well.
  uint32_t NumLeftover = 0;
  SmallVector<std::string, 8> LeftoverFuncs;
  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &BF = BFI.second;
    for (BinaryBasicBlock &BB : BF) {
      for (MCInst &Inst : BB) {
        if (BC.MIB->isADRP(Inst))
          continue;
        for (unsigned OpIdx = 0; OpIdx < Inst.getNumOperands(); ++OpIdx) {
          const MCSymbol *Sym = BC.MIB->getTargetSymbol(Inst, OpIdx);
          if (Sym && Sym->getName() == "__BOLT_got_zero") {
            ++NumLeftover;
            if (LeftoverFuncs.size() < 8)
              LeftoverFuncs.push_back(BF.getPrintName());
          }
        }
      }
    }
  }
  if (NumLeftover) {
    errs() << "BOLT-ERROR: " << NumLeftover
           << " unretargetable __BOLT_got_zero reference(s) remain after "
              "FixRelaxations (e.g. ";
    for (const std::string &F : LeftoverFuncs)
      errs() << F << " ";
    errs() << "). The output binary would contain stale GOT offsets.\n";
    exit(1);
  }
}

} // namespace bolt
} // namespace llvm
