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

#define DEBUG_TYPE "bolt-golang-postprocess"

using namespace llvm;

namespace opts {
extern cl::opt<bool> GolangAnnotationChecker;
}

namespace llvm {
namespace bolt {

void GolangPostPass::skipPleaseUseCallersFramesPass(BinaryContext &BC) {
  // The function must have sizeofSkipFunction bytes of nops
  // runtime/traceback.go
  const int sizeofSkipFunction = 256;
  const char *FunctionName = "runtime.skipPleaseUseCallersFrames";
  BinaryFunction *Function = BC.getBinaryFunctionByName(FunctionName);
  if (!Function) {
    LLVM_DEBUG(outs() << "BOLT-WARNING: Failed to get " << FunctionName
                      << "\n");
    return;
  }

  assert(Function->getLayout().block_begin() !=
             Function->getLayout().block_end() &&
         "Unexpected empty function");
  BinaryBasicBlock *BB = *Function->getLayout().block_begin();
  BB->clear();

  MCInst Inst;
  BC.MIB->createNoop(Inst);
  uint64_t Size = sizeofSkipFunction / BC.computeInstructionSize(Inst);
  std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
  unsigned PcspIndex = GoFunc->getPcspIndex();
  while (Size--) {
    Inst.clear();
    BC.MIB->createNoop(Inst);
    BB->insertInstruction(BB->begin(), Inst);
    addVarintAnnotation(BC, *BB->begin(), PcspIndex, /* Value */ 0,
                        /*IsNext*/ false);
  }
}

void GolangPostPass::instrumentExitCall(BinaryContext &BC) {
  // Golang doesn not call anything on process termination
  // Insert instrumentation fini call at exit function
  if (!opts::Instrument)
    return;
  StringRef GoExitName = getModule()->getRuntimeExitName();
  BinaryFunction *Function = BC.getBinaryFunctionByName(GoExitName);
  if (!Function) {
    outs() << "BOLT-WARNING: Failed to get runtime.exit for instrumentation!\n";
    return;
  }

  if (!BC.shouldEmit(*Function)) {
    outs() << "BOLT-WARNING: runtime.exit could not be patched for "
              "instrumentation!\n";
    return;
  }

  assert(Function->getLayout().block_begin() !=
             Function->getLayout().block_end() &&
         "runtime.exit is empty");
  BinaryBasicBlock *BB = *Function->getLayout().block_begin();
  MCSymbol *FiniHandler =
      BC.Ctx->getOrCreateSymbol("__bolt_trampoline_instr_fini_call");
  std::vector<MCInst> Instrs = BC.MIB->createInstrumentFiniCall(
      FiniHandler, &*BC.Ctx, /*IsTailCall*/ false);
  BB->insertInstructions(BB->begin(), Instrs);
}

uint32_t GolangPostPass::pcdataPass(BinaryFunction *BF, GoFunc *GoFunc,
                                    const uint32_t Index,
                                    const unsigned AllocId) {
  int Ret;
  int32_t Val, NextVal;
  MCInst NoopInst;
  BinaryContext &BC = BF->getBinaryContext();
  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    for (uint64_t I = 0; I < BB->size(); ++I) {
      MCInst &Inst = BB->getInstructionAtIndex(I);
      bool IsMap = hasVarintAnnotation(BC, Inst, Index);
      if (!IsMap)
        continue;

      if (Index == GoFunc->getPcdataStackMapIndex()) {
        auto addNoop = [&](const int32_t Val, const int32_t NextVal) {
          BC.MIB->createNoop(NoopInst);
          addVarintAnnotation(BC, NoopInst, Index, Val, /*IsNext*/ false,
                              AllocId);
          addVarintAnnotation(BC, NoopInst, Index, NextVal, /*IsNext*/ true,
                              AllocId);
          auto NextIt = std::next(BB->begin(), I + 1);
          BB->insertInstruction(NextIt, NoopInst);
        };

        NextVal = getVarintAnnotation(BC, Inst, Index, /*IsNext*/ true);
        int32_t NextInstVal;
        Ret = getNextMCinstVal(BBIt, I, Index, NextInstVal, nullptr);
        if (Ret < 0) {
          Val = getVarintAnnotation(BC, Inst, Index);
          // Check that the last instruction value equals next value
          if (Val != NextVal)
            addNoop(NextVal, NextVal);

          return 0;
        }

        // We need to save original chain of values for call instructions so
        // check that the value of the next instuction is the same as expected
        if (NextVal == NextInstVal || !BC.MIB->isCall(Inst))
          continue;

        // If the Value is not the same as expected create nop
        // with the right values
        addNoop(NextVal, NextInstVal);
      }
    }
  }

  return 0;
}

int GolangPostPass::pclntabPass(BinaryContext &BC) {
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
  const uint8_t *SectionBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  ParallelUtilities::WorkFuncWithAllocTy WorkFun =
      [&](BinaryFunction &Function, MCPlusBuilder::AllocatorIdTy AllocId) {
        uint64_t FuncOffset = Function.getGolangFunctabOffset();
        std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
        GoFunc->read(BC, SectionBuffer, IsLittleEndian, nullptr, &FuncOffset);
        GoFunc->setModule(getModule());
        auto getPcdata = [&](const uint32_t Index, bool Force = false) {
          uint32_t MapOffsetVal = GoFunc->getPcdata(Index);
          if (!MapOffsetVal && !Force)
            return;

          pcdataPass(&Function, GoFunc.get(), Index, AllocId);
        };

        getPcdata(GoFunc->getPcdataStackMapIndex());

        if (Function.getLayout().block_begin() !=
            Function.getLayout().block_end()) {
          // Insert NOP to the end of function, if it ends with call instruction
          // to provide correct PCSP table lately for runtime.gentraceback
          // This is needed for rare cases with no-return calls, since the
          // pcvalue searches for targetpc < pc and for tail cals we will have
          // targetpc == pc. We could also add +1 in PCSP final offset, but
          // it won't fix the preserve PCSP table case, so this solution seems
          // to be more robust
          BinaryBasicBlock *BBend = *Function.getLayout().block_rbegin();
          if (BC.MIB->isCall(*BBend->rbegin())) {
            MCInst NoopInst;
            BC.MIB->createNoop(NoopInst);
            if (opts::GolangAnnotationChecker)
              BC.MIB->addAnnotation(NoopInst, "GoInstMarker", true);
            BBend->insertInstruction(BBend->end(), NoopInst);
          }
        }
      };
  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_QUADRATIC, WorkFun,
      skipFunc, "pcdataGoPostProcess", /*ForceSequential*/ true);
  return 0;
}

void GolangPostPass::runOnFunctions(BinaryContext &BC) {
  int Ret;

  {
    NamedRegionTimer T("post-skipPleaseUseCallersFramesPass",
                       "golang postprocess skipPleaseUseCallersFramesPass",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    skipPleaseUseCallersFramesPass(BC);
  }

  {
    NamedRegionTimer T(
        "post-instrumentExitCall", "golang postprocess instrumentExitCall",
        GolangTimerGroupName, GolangTimerGroupDesc, opts::TimeOpts);
    instrumentExitCall(BC);
  }

  {
    NamedRegionTimer T("post-pclntabPass", "golang postprocess pclntabPass",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    Ret = pclntabPass(BC);
  }
  if (Ret < 0) {
    errs() << "BOLT-ERROR: Golang postprocess pclntab pass failed!\n";
    exit(1);
  }
}

} // end namespace bolt
} // end namespace llvm
