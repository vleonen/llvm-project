//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Passes/Golang.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/Support/Timer.h"

#define DEBUG_TYPE "bolt-golang-preprocess"

using namespace llvm;

using namespace llvm;
using namespace bolt;

namespace opts {
extern cl::opt<bool> NoThreads;
extern cl::opt<bool> GolangAnnotationChecker;
}

static void inlTreePass(BinaryFunction *Function, GoFunc *GoFunc,
                        const unsigned AllocId, const MCCodeEmitter *Emitter) {
  // inlTreePass: Reads inline call tree from funcdata section
  //
  // Input buffer layout (funcdata section at FuncdataAddr + I *
  // sizeof(InlinedCall)):
  //   See InlinedCall_v1_20_7 binary layout in go_v1_20.h (12 bytes total)
  //   Used to annotate inlined function call sites for stack unwinding
  BinaryContext &BC = Function->getBinaryContext();
  const unsigned PcdataIndex = GoFunc->getPcdataInlTreeIndex();
  int32_t MaxVal = GoFunc->getPcdataMax(PcdataIndex);
  if (MaxVal < 0)
    return;

  const unsigned Index = GoFunc->getFuncdataInlTreeIndex();
  if (GoFunc->getNfuncdata() <= Index)
    return;

  uint64_t FuncdataAddr = GoFunc->getFuncdata(Index);
  if (!FuncdataAddr)
    return;

  struct InlinedCall_v1_20_7 InlinedCall_v1_20_7;

  ErrorOr<BinarySection &> FuncdataSection =
      BC.getSectionForAddress(FuncdataAddr);
  if (!FuncdataSection)
    return;

  const uint8_t *FuncdataBuffer =
      reinterpret_cast<const uint8_t *>(FuncdataSection->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  for (uint32_t I = 0; I < (uint32_t)MaxVal + 1; ++I) {
    uint64_t Offset = (uint64_t)(FuncdataAddr - FuncdataSection->getAddress());
    Offset += I * sizeof(InlinedCall_v1_20_7);

    const uint8_t *ReadPtr = FuncdataBuffer + Offset;
    InlinedCall_v1_20_7.FuncID = readEndianValRaw(&ReadPtr, IsLittleEndian, 1);
    InlinedCall_v1_20_7.Unused[0] =
        readEndianValRaw(&ReadPtr, IsLittleEndian, 1);
    InlinedCall_v1_20_7.Unused[1] =
        readEndianValRaw(&ReadPtr, IsLittleEndian, 1);
    InlinedCall_v1_20_7.Unused[2] =
        readEndianValRaw(&ReadPtr, IsLittleEndian, 1);
    InlinedCall_v1_20_7.NameOff =
        (int32_t)readEndianValRaw(&ReadPtr, IsLittleEndian, 4);
    InlinedCall_v1_20_7.ParentPc =
        (int32_t)readEndianValRaw(&ReadPtr, IsLittleEndian, 4);
    InlinedCall_v1_20_7.StartLine =
        (int32_t)readEndianValRaw(&ReadPtr, IsLittleEndian, 4);

    for (BinaryBasicBlock *BB : Function->getLayout().blocks()) {
      if (InlinedCall_v1_20_7.ParentPc >= BB->getEndOffset())
        continue;

      uint32_t Offset = BB->getOffset();
      for (MCInst &II : *BB) {
        if (auto StoredOffset = BC.MIB->getOffset(II)) {
          Offset = *StoredOffset;
          assert(Offset != std::numeric_limits<uint32_t>::max() &&
                 "Invalid offset");
        }

        if (Offset < InlinedCall_v1_20_7.ParentPc) {
          Offset += BC.computeInstructionSize(II, Emitter);
          continue;
        }

        assert(Offset == InlinedCall_v1_20_7.ParentPc && "Offset overflow");

        // NOTE Annotations must not be created with in concurrent threads
        static std::atomic_flag Lock = ATOMIC_FLAG_INIT;
        while (Lock.test_and_set(std::memory_order_acquire))
          ;
        addFuncdataAnnotation(BC, II, Index, I, AllocId);
        Lock.clear(std::memory_order_release);
        // To be able to restore right inline unwinding we will lock the
        // instruction
        bool &Locked = BC.MIB->getOrCreateAnnotationAs<bool>(II, "Locked");
        Locked = true;
        break;
      }

      break;
    }
  }
}

static uint32_t readPcdataPass(BinaryFunction *Function, GoFunc *GoFunc,
                               const uint8_t *Data, uint64_t *MapOffset,
                               const uint32_t Index, const uint8_t Quantum,
                               const unsigned AllocId,
                               const MCCodeEmitter *Emitter) {
  BinaryContext &BC = Function->getBinaryContext();
  int32_t ValSum = -1, MaxVal = -1;
  uint64_t EndVarintRange = 0;
  uint32_t Offset = 0;
  MCInst *PrevII = nullptr;
  bool IsFirst = true;
  int32_t Val;

  for (BinaryBasicBlock *BB : Function->getLayout().blocks()) {
    assert(Offset <= BB->getOffset() &&
           "Offset of beginning of BB should be not higher than calculated "
           "offset in readPcdataPass");
    Offset = BB->getOffset();
    for (MCInst &II : *BB) {
      if (auto StoredOffset = BC.MIB->getOffset(II)) {
        Offset = *StoredOffset;
        assert(Offset != std::numeric_limits<uint32_t>::max() &&
               "Invalid offset");
      }
      if (Offset >= EndVarintRange) {
        // Update offsets to effective varint range
        Val = readVarintPair(Data, MapOffset, ValSum, EndVarintRange, Quantum);
        if (ValSum > MaxVal)
          MaxVal = ValSum;
        LLVM_DEBUG(dbgs() << "BOLT-DEBUG: readPcdataPass NEXT Function="
                          << *Function << " Offset=0x"
                          << Twine::utohexstr(Offset) << " Index=" << Index
                          << " ValSum=" << ValSum << " Val=" << Val << "\n");
        if (!Val && !IsFirst)
          break;
      }
      assert(Offset < EndVarintRange && "Offset overflow");

      addVarintAnnotation(BC, II, Index, ValSum, /*IsNext*/ false, AllocId);
      if (Index == GoFunc->getPcdataStackMapIndex() && PrevII)
        addVarintAnnotation(BC, *PrevII, Index, ValSum, /*IsNext*/ true,
                            AllocId);
      LLVM_DEBUG(dbgs() << "BOLT-DEBUG: readPcdataPass Function=" << *Function
                        << " Offset=0x" << Twine::utohexstr(Offset)
                        << " Index=" << Index << " ValSum=" << ValSum
                        << " Val=" << Val << "\n");
      PrevII = &II;
      Offset += BC.computeInstructionSize(II, Emitter);
    }
    LLVM_DEBUG(BB->dump());
    IsFirst = false;
  }

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: readPcdataPass END Function=" << *Function
                    << " Offset=0x" << Twine::utohexstr(Offset) << " Index="
                    << Index << " ValSum=" << ValSum << " Val=" << Val << "\n");

  if (Index == GoFunc->getPcdataStackMapIndex())
    addVarintAnnotation(BC, *PrevII, Index, ValSum, /*IsNext*/ true, AllocId);

  return MaxVal;
}

void GolangPrePass::deferreturnPass(BinaryFunction &BF,
                                    const uint64_t DeferOffset,
                                    const unsigned AllocId,
                                    const MCCodeEmitter *Emitter) {
  BinaryContext &BC = BF.getBinaryContext();
  uint64_t Offset = 0;
  for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
    for (MCInst &II : *BB) {
      if (auto StoredOffset = BC.MIB->getOffset(II)) {
        Offset = *StoredOffset;
        assert(Offset != std::numeric_limits<uint32_t>::max() &&
               "Invalid offset");
      }

      if (Offset < DeferOffset) {
        Offset += BC.computeInstructionSize(II, Emitter);
        continue;
      }

      if (Offset != DeferOffset)
        break;

      assert(BC.MIB->isCall(II));
      BC.MIB->addAnnotation(II, "IsDefer", true, AllocId);
      return;
    }
  }

  outs() << "Deferreturn call was not found for " << BF << "\n";
  exit(1);
}

int GolangPrePass::pclntabPass(BinaryContext &BC) {
  const uint64_t PclntabAddr = getPcHeaderAddr();
  if (!PclntabAddr) {
    errs() << "BOLT-ERROR: Pclntab address is zero!\n";
    return -1;
  }

  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabAddr);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: Failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  const class Pclntab *PclntabInstance = getPclntab();
  uint64_t Offset = PclntabInstance->getPclntabOffset();
  const uint8_t *SectionBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();
  for (uint64_t F = 0; F < PclntabInstance->getFunctionsCount(); ++F) {
    RemoveRelaReloc(BC, Section, Offset);
    struct Functab Functab =
        PclntabInstance->getFuncTabRaw(SectionBuffer, IsLittleEndian, &Offset);

    // Find absolute address of Golang function
    Functab.Address = getGolangFunctionAddress(Functab.Address);

    BinaryFunction *Function = BC.getBinaryFunctionAtAddress(Functab.Address);
    if (!Function) {
      outs() << "Failed to find function by address "
             << Twine::utohexstr(Functab.Address) << "\n";
      return -1;
    }

    Function->setGolangFunctabOffset(PclntabInstance->getFunctabOffset() +
                                     Functab.Offset);
  }

  // Remove maxpc relocation (last pclntab entry)
  RemoveRelaReloc(BC, Section, Offset);

  ParallelUtilities::WorkFuncWithAllocTy WorkFun =
      [&](BinaryFunction &Function, MCPlusBuilder::AllocatorIdTy AllocId) {
        const uint8_t *SectionBuffer =
            reinterpret_cast<const uint8_t *>(Section->getContents().data());
        const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();
        const class Pclntab *Pclntab = getPclntab();
        if (Function.getLayout().block_begin() ==
            Function.getLayout().block_end())
          return;

        BinaryContext::IndependentCodeEmitter Emitter;
        if (!opts::NoThreads) {
          Emitter =
              Function.getBinaryContext().createIndependentMCCodeEmitter();
        }

        uint64_t FuncOffset = Function.getGolangFunctabOffset();
        std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
        GoFunc->read(BC, SectionBuffer, IsLittleEndian, Section, &FuncOffset);
        GoFunc->setModule(getModule());

        {
          // Remove funcdata relocations
          uint32_t Foffset = GoFunc->getFuncdataOffset();
          for (int I = 0; I < GoFunc->getNfuncdata(); ++I) {
            RemoveRelaReloc(BC, Section, Foffset);
            Foffset += getPsize();
          }
        }

        if (!GoFunc->getNfuncdata()) {
          Function.setPreserveNops(true);
          Function.setIsAsm(true);
        }

        // Special functions that we must not change
        for (StringRef Name : Function.getNames()) {
          if (GoFunc->hasReservedID(Name.str())) {
            Function.setSimple(false);
            Function.setPreserveNops(true);
            break;
          }
          for (const std::string &SkipName : opts::DefaultSkipGolangFuncs) {
            if (Name.contains(SkipName)) {
              Function.setSimple(false);
              Function.setPreserveNops(true);
              break;
            }
          }
          for (const std::string &SkipName : opts::SkipGolangFuncs) {
            if (Name.contains(SkipName)) {
              Function.setSimple(false);
              Function.setPreserveNops(true);
              break;
            }
          }
        }

        auto GetPcdata = [&](const uint32_t Index) -> bool {
          if (Index >= GoFunc->getNpcdata())
            return false;

          int32_t Max = -1;
          uint32_t MapOffsetVal = GoFunc->getPcdata(Index);
          if (MapOffsetVal) {
            uint64_t MapOffset = Pclntab->getPctabOffset() + MapOffsetVal;
            Max = readPcdataPass(&Function, GoFunc.get(), SectionBuffer,
                                 &MapOffset, Index, Pclntab->getQuantum(),
                                 AllocId, Emitter.MCE.get());
          }

          if (Max != -1 && Index == GoFunc->getPcdataInlTreeIndex())
            GoFunc->setPcdataMaxVal(Index, Max);

          return !!MapOffsetVal;
        };

        if (GoFunc->getNpcdata() &&
            !GetPcdata(GoFunc->getPcdataUnsafePointIndex())) {
          // The function has no PCDATA_UnsafePoint info, so we will mark every
          // instruction as a safe one.
          const int SafePoint = GoFunc->getPcdataSafePointVal();
          const uint32_t Index = GoFunc->getPcdataUnsafePointIndex();
          for (BinaryBasicBlock *BB : Function.getLayout().blocks())
            for (auto II = BB->begin(); II != BB->end(); ++II)
              addVarintAnnotation(BC, *II, Index, SafePoint, /*IsNext*/ false,
                                  AllocId);
        }

        if (GoFunc->getNpcdata()) {
          GetPcdata(GoFunc->getPcdataStackMapIndex());
          GetPcdata(GoFunc->getPcdataInlTreeIndex());
          if (auto Index = GoFunc->getPcdataArgLiveIndex())
            GetPcdata(*Index);
        }

        uint64_t DeferOffset = GoFunc->getDeferreturnOffset();
        if (DeferOffset)
          deferreturnPass(Function, DeferOffset, AllocId, Emitter.MCE.get());

        // ASM Functions might use the system stack and we won't be able to
        // locate that the stack was switched.
        uint64_t Offset = Pclntab->getPctabOffset() + GoFunc->getPcspOffset();
        readPcdataPass(&Function, GoFunc.get(), SectionBuffer, &Offset,
                       GoFunc->getPcspIndex(), Pclntab->getQuantum(), AllocId,
                       Emitter.MCE.get());
        inlTreePass(&Function, GoFunc.get(), AllocId, Emitter.MCE.get());
      };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_QUADRATIC, WorkFun,
      skipFunc, "pcdataGoPreProcess", /*ForceSequential*/ true);

  return 0;
}

int GolangPrePass::goPassInit(BinaryContext &BC) {
  BC.MIB->getOrCreateAnnotationIndex("IsDefer");

  // Initialize annotation index for multi-thread access
  std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
  auto initAnnotation = [&](const unsigned Index) {
    BC.MIB->getOrCreateAnnotationIndex(getVarintName(Index));
    BC.MIB->getOrCreateAnnotationIndex(getVarintName(Index, /*IsNext*/ true));
  };

  initAnnotation(GoFunc->getPcdataUnsafePointIndex());
  initAnnotation(GoFunc->getPcdataStackMapIndex());
  initAnnotation(GoFunc->getPcdataInlTreeIndex());
  initAnnotation(GoFunc->getPcspIndex());
  return 0;
}

int GolangPrePass::nopPass(BinaryContext &BC) {
  // The golang might generate unreachable jumps e.g.
  // https://go-review.googlesource.com/c/go/+/380894/
  // Removing the nops at branch destination might affect PCSP table generation
  // for the code below the nop. Remove NOP instruction annotation at the
  // beginning of the basic block in order to preserve BB layout for such cases.
  // Shorten multi-byte NOP before annotation remove.

  ParallelUtilities::WorkFuncWithAllocTy WorkFun =
      [&](BinaryFunction &Function, MCPlusBuilder::AllocatorIdTy AllocId) {
        for (BinaryBasicBlock *BB : Function.getLayout().blocks()) {
          MCInst &Inst = BB->front();
          if (!BC.MIB->isNoop(Inst))
            continue;

          BC.MIB->shortenInstruction(Inst, *BC.STI);
          BC.MIB->removeAnnotation(Inst, "NOP");
          BC.MIB->removeAnnotation(Inst, "Size");
        }
      };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_QUADRATIC, WorkFun,
      skipFunc, "nopGoPreProcess", /*ForceSequential*/ true);
  return 0;
}

static int goAnnotationMarker(BinaryContext &BC) {
  BC.MIB->getOrCreateAnnotationIndex("GoInstMarker");
  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
      for (MCInst &II : *BB) {
        BC.MIB->addAnnotation(II, "GoInstMarker", true);
      }
    }
  };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, skipFunc,
      "goAnnotationMarker",
      /*ForceSequential*/ true);
  return 0;
}

void GolangPrePass::runOnFunctions(BinaryContext &BC) {
  int Ret;

#define CALL_STAGE(func)                                                       \
  {                                                                            \
    NamedRegionTimer T("pre-" #func, "golang preprocess " #func,               \
                       GolangTimerGroupName, GolangTimerGroupDesc,             \
                       opts::TimeOpts);                                        \
    Ret = func(BC);                                                            \
  }                                                                            \
  if (Ret < 0) {                                                               \
    errs() << "BOLT-ERROR: golang preprocess " << #func << " stage failed!\n"; \
    exit(1);                                                                   \
  }

  CALL_STAGE(goPassInit);
  CALL_STAGE(pclntabPass)
  CALL_STAGE(nopPass);
  if (opts::GolangAnnotationChecker) {
    CALL_STAGE(goAnnotationMarker);
  }
}