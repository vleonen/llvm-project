//===--------- Passes/Golang.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Golang binaries support passes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_GOLANG_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_GOLANG_H

#include "BinaryPasses.h"
#include "bolt/Rewrite/MetadataRewriter.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

#include "Golang/go_v1_20.h"
#include "Golang/go_v1_22.h"
#include "Golang/go_v1_24.h"

using namespace llvm;

namespace opts {
enum GolangVersion : char {
  GV_NONE = 0,
  GV_FIRST,
  GV_AUTO = GV_FIRST,
  GV_1_20, /// Version family 1.20.x (was GV_1_20_7)
  GV_1_22, /// Version family 1.22.x (was GV_1_22_1)
  GV_1_24, /// Version family 1.24.x (was GV_1_24_6)
  GV_LAST,
  GV_LATEST = GV_LAST - 1 /// Latest version family
};

extern cl::opt<opts::GolangVersion> GolangPass;
extern cl::opt<bool> GolangPreserveFunctions;
extern cl::opt<bool> GolangRemoveOldPclntab;
extern cl::list<std::string> SkipGolangFuncs;
extern const std::vector<std::string> DefaultSkipGolangFuncs;
} // namespace opts

bool shouldSkipGolangFunc(StringRef FuncName);

namespace llvm {
namespace bolt {

class BinaryContext;
class BinaryFunction;
class BinarySection;
class MetadataRewriter;
std::unique_ptr<MetadataRewriter>
createGolangMetadataRewriter(BinaryContext &BC);

extern const char GolangTimerGroupName[];
extern const char GolangTimerGroupDesc[];

class GolangBinaryInfo {
public:
  static GolangBinaryInfo &getInstance();

  void initialize(BinaryContext &BC);
  bool isInitialized() const { return Initialized; }

  int checkGoVersion(BinaryContext &BC);
  int getSymbols(BinaryContext &BC);
  int textSectMapReadPass(BinaryContext &BC);
  int textsectmapPass(BinaryContext &BC);
  uint64_t getGolangFunctionAddress(uint64_t Address);
  int patch(BinaryContext &BC);

  struct Module *getModule() { return FirstModule.get(); }
  const struct Module *getModule() const { return FirstModule.get(); }
  class Pclntab *getPclntab() { return Pclntab.get(); }
  const class Pclntab *getPclntab() const { return Pclntab.get(); }
  const std::vector<TextsecmapStruct> &getTextsecmap() const {
    return Textsecmap;
  }
  uint64_t getPcHeaderAddr() const;
  uint8_t getPsize() const { return Pclntab ? Pclntab->getPsize() : 0; }
  bool hasSwissMap() const { return HasSwissMap; }

  BinarySection *getFindfunctabSection() const { return FindfunctabSection; }
  void setFindfunctabSection(BinarySection *Section) {
    FindfunctabSection = Section;
  }

  BinarySection *getPclntabSection() const { return PclntabSection; }
  void setPclntabSection(BinarySection *Section) { PclntabSection = Section; }

private:
  GolangBinaryInfo() = default;
  static GolangBinaryInfo Instance;

  bool Initialized = false;
  std::unique_ptr<struct Module> FirstModule;
  std::unique_ptr<class Pclntab> Pclntab;
  std::vector<TextsecmapStruct> Textsecmap;
  bool HasSwissMap = false;
  BinarySection *FindfunctabSection = nullptr;
  BinarySection *PclntabSection = nullptr;
};

/// Ordering indexes assigned to Go functions before layout, so that
/// runtime.text is placed first and runtime.etext last (see
/// GolangPass::assignGoFunctionIndexes).
enum GoIndexOrder {
  GO_FIRST_BF_INDEX = 0,
  FIRST_BF_INDEX = 1, // first index for sorted functions (after runtime.text)
  GO_UNUSED_BF_INDEX = -3U,
  GO_LAST_BF_INDEX = -2U,
};

class GolangPass : public BinaryFunctionPass {
public:
  /// Golang version strings
  static const char *GolangStringVer[opts::GV_LAST];

  explicit GolangPass(BinaryContext &BC) : BinaryFunctionPass(false) {
    if (!GolangBinaryInfo::getInstance().isInitialized())
      GolangBinaryInfo::getInstance().initialize(BC);
  }

  static const char *getFirstBFName(void) {
    const char *const Name = "runtime.text";
    return Name;
  }

  static const char *getLastBFName(void) {
    const char *const Name = "runtime.etext";
    return Name;
  }

  static uint32_t getUndAarch64(void) { return 0xbea71700; }

  /// Assign ordering indexes to Go functions in \p BFs before layout:
  /// runtime.text = GO_FIRST_BF_INDEX (0), unsorted Go functions =
  /// GO_UNUSED_BF_INDEX (-3), runtime.etext = GO_LAST_BF_INDEX (-2)
  /// and non-Go functions = INVALID_BF_INDEX (-1).
  static void assignGoFunctionIndexes(std::map<uint64_t, BinaryFunction> &BFs);

  static void
  annotateInstrumentationInstructions(InstructionListType &Instrs,
                                      BinaryBasicBlock &BB,
                                      BinaryBasicBlock::iterator Iter);

  uint64_t getPcHeaderAddr() const {
    return GolangBinaryInfo::getInstance().getPcHeaderAddr();
  }

  uint8_t getPsize() const {
    return GolangBinaryInfo::getInstance().getPsize();
  }

  static bool hasMorestackBlock(const BinaryFunction &Function);

  static bool hasIndirectCall(const BinaryFunction &Function);

  static std::optional<uint32_t>
  computeGoFunctionFrameSize(const BinaryFunction &Function);

  static const MCSymbol *
  findMorestackBranchTarget(const BinaryFunction &Function);

  static bool shouldSkipExtendStack(const BinaryContext &BC,
                                    const BinaryFunction &Function);

  class Pclntab *getPclntab() {
    return GolangBinaryInfo::getInstance().getPclntab();
  }

  const class Pclntab *getPclntab() const {
    return GolangBinaryInfo::getInstance().getPclntab();
  }

  struct Module *getModule() {
    return GolangBinaryInfo::getInstance().getModule();
  }

  const struct Module *getModule() const {
    return GolangBinaryInfo::getInstance().getModule();
  }

  const std::vector<TextsecmapStruct> &getTextsecmap() const {
    return GolangBinaryInfo::getInstance().getTextsecmap();
  }

  const char *getName() const override { return "golang"; }

  uint64_t getGolangFunctionAddress(uint64_t Address) {
    return GolangBinaryInfo::getInstance().getGolangFunctionAddress(Address);
  }

  /// Pass entry point
  Error runOnFunctions(BinaryContext &BC) override;
  int getNextMCinstVal(FunctionLayout::block_iterator BBIt, uint64_t I,
                       const uint32_t Index, int32_t &Val,
                       uint64_t *NextOffset);
  void updRestartVarintPass(BinaryContext &BC, BinaryFunction *BF,
                            GoFunc *GoFunc);
  int unsafePointPass(BinaryFunction *BF, GoFunc *GoFunc);
  int pcspInstPass(BinaryFunction *BF, GoFunc *GoFunc);
  int pclntabPass(BinaryContext &BC);
  int findFuncTabPass(BinaryContext &BC);
  int processPcdata(BinaryContext &BC, BinaryFunction *BF, GoFunc *GoFunc,
                    uint8_t *SectionData, uint8_t **DataFuncOffset);
};

class GolangPostPass : public GolangPass {
public:
  explicit GolangPostPass(BinaryContext &BC) : GolangPass(BC) {}
  const char *getName() const override { return "golang-post"; }

  /// Pass entry point
  Error runOnFunctions(BinaryContext &BC) override;
  void skipPleaseUseCallersFramesPass(BinaryContext &BC);
  void instrumentExitCall(BinaryContext &BC);
  uint32_t pcdataPass(BinaryFunction *BF, GoFunc *GoFunc, const uint32_t Index,
                      const unsigned AllocId);
  int pclntabPass(BinaryContext &BC);
};

class GolangPrePass : public GolangPass {
public:
  explicit GolangPrePass(BinaryContext &BC) : GolangPass(BC) {}
  const char *getName() const override { return "golang-pre"; }

  /// Pass entry point
  Error runOnFunctions(BinaryContext &BC) override;
  int goPassInit(BinaryContext &BC);
  int nopPass(BinaryContext &BC);
  int pclntabPass(BinaryContext &BC);
  void deferreturnPass(BinaryFunction &BF, const uint64_t DeferOffset,
                       const unsigned AllocId, const MCCodeEmitter *Emitter);
};

class GolangMetadataRewriter final : public MetadataRewriter {
public:
  struct StackVal {
    uint32_t Size;
    int32_t OldVal;
    int32_t Val;
  };
  using InstBias = std::map<uint32_t, struct StackVal>;

  explicit GolangMetadataRewriter(StringRef Name, BinaryContext &BC);

  Error preCFGInitializer() override;
  Error postCFGInitializer() override;
  Error postEmitFinalizer() override;

  int findFuncTabPass(BinaryContext &BC);
  int pclntabPass(BinaryContext &BC);

  /// Register a 32-bit metadata patch into \p Section at section-relative
  /// \p Offset. Applied by the flushPendingRelocations loop in
  /// RewriteInstance (post-restore in -rewrite mode).
  static void addMetadataPatch(BinarySection &Section, uint64_t Offset,
                               uint32_t Value);

  int processPcdata(BinaryContext &BC, BinaryFunction *BF, GoFunc *GoFunc,
                    uint8_t *SectionData, uint8_t **DataFuncOffset);
  int getNextMCinstVal(FunctionLayout::block_iterator BBIt, uint64_t I,
                       const uint32_t Index, int32_t &Val,
                       uint64_t *NextOffset);
  uint32_t deferreturnPass(BinaryContext &BC, BinaryFunction *Function);
  void updRestartVarintPass(BinaryContext &BC, BinaryFunction *BF,
                            GoFunc *GoFunc);
  int writeVarintPass(BinaryFunction *BF, GoFunc *GoFunc,
                      uint8_t **DataFuncOffset, const uint32_t Index,
                      const uint8_t Quantum);
  int unsafePointPass(BinaryFunction *BF, GoFunc *GoFunc);
  int pcspInstPass(BinaryFunction *BF, GoFunc *GoFunc);
  uint64_t getBFInstrOffset(const BinaryBasicBlock *BB, const MCInst *Instr);
  void writeVarint(uint8_t *Data, uint64_t *Offset, uint32_t Val);
  void writeVarint(uint8_t **Data, uint32_t Val);
  void writeVarintPair(int32_t Val, int32_t &PrevVal, uint64_t Offset,
                       uint64_t &CurrentOffset, bool &IsFirst,
                       uint8_t **DataFuncOffset, const uint8_t Quantum,
                       const char *FuncName);
  uint32_t stackCounter(BinaryFunction *BF, BinaryBasicBlock *BB, InstBias &Map,
                        uint32_t SpVal);

  void inlTreePass(BinaryFunction *BF, GoFunc *GoFunc);
  void wrapInfoPass(BinaryFunction *BF, GoFunc *GoFunc,
                    BinaryFunction *FirstBF);

private:
  struct TypeProcessingState {
    std::vector<uint64_t> DiscoveredTypes;
    std::mutex QueueMutex;
    std::condition_variable QueueCondVar;
    std::unordered_set<uint64_t> VisitedTypes;
    llvm::sys::RWMutex VisitedMutex;
    std::atomic<size_t> NextIndex{0};
    std::atomic<bool> Error{false};
    std::atomic<unsigned> ActiveWorkers{0};
    /// Output address of the first Go function (runtime.text), loop-invariant
    /// for the whole pass; computed once by typelinksPass.
    uint64_t TextStartAddr = 0;
  };

  static int typePass(BinaryContext &BC, uint64_t TypeAddr,
                      TypeProcessingState &State);
  int typelinksPass(BinaryContext &BC);
  std::unordered_map<const BinaryBasicBlock *, uint64_t> BBSizes;
};

} // namespace bolt
} // namespace llvm

#endif
