//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/Golang.h"
#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Passes/Golang/go_common.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Timer.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#define DEBUG_TYPE "bolt-golang"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

extern cl::opt<bool> Instrument;
extern cl::opt<bool> NoHugePages;
extern cl::opt<unsigned> AlignFunctions;
cl::opt<bool>
    GolangAnnotationChecker("golang-annotation-checker",
                            cl::desc("Perform additional annotations presence "
                                     "checks for output instructions"),
                            cl::init(false), cl::ZeroOrMore, cl::Hidden,
                            cl::cat(BoltOptCategory));

extern cl::opt<unsigned> AlignFunctionsMaxBytes;
extern cl::opt<bool> UseCompactAligner;
} // end namespace opts

namespace llvm {
namespace bolt {

const char GolangTimerGroupName[] = "Golang";
const char GolangTimerGroupDesc[] = "Golang passes breakdown";

const char *GolangPass::GolangStringVer[opts::GV_LAST] = {
    "none", "auto", "go1.20", "go1.22", "go1.24",
};

bool shouldSkipGolangFunc(StringRef FuncName) {
  for (const std::string &SkipName : opts::DefaultSkipGolangFuncs) {
    if (FuncName.contains(SkipName))
      return true;
  }
  for (const std::string &SkipName : opts::SkipGolangFuncs) {
    if (FuncName.contains(SkipName))
      return true;
  }
  return false;
}

// Mask to extract type kind from type flags
// High 3 bits are for tflag, low 5 bits are for kind
// Matches Go runtime: runtime/typekind.go
#define KINDMASK ((1 << 5) - 1)
// Flag indicating uncommon type information is present (e.g., methods)
// Matches Go runtime: runtime/type.go
#define UNCOMMON_FLAG (1 << 0)

// reflect/type.go ; runtime/typekind.go
enum Kind {
  Invalid = 0,
  Bool,
  Int,
  Int8,
  Int16,
  Int32,
  Int64,
  Uint,
  Uint8,
  Uint16,
  Uint32,
  Uint64,
  Uintptr,
  Float32,
  Float64,
  Complex64,
  Complex128,
  Array,
  Chan,
  Func,
  Interface,
  Map,
  Ptr,
  Slice,
  String,
  Struct,
  UnsafePointer,
  LastKind
};

BinaryFunction *getBF(BinaryContext &BC, std::vector<BinaryFunction *> &BFs,
                      const char *Name) {
  for (auto BFit = BFs.rbegin(); BFit != BFs.rend(); ++BFit) {
    BinaryFunction *BF = *BFit;
    if (BF->hasRestoredNameRegex(Name))
      return BF;
  }

  return nullptr;
}

BinaryFunction *getFirstBF(BinaryContext &BC,
                           std::vector<BinaryFunction *> &BFs) {
  return getBF(BC, BFs, GolangPass::getFirstBFName());
}

BinaryFunction *getLastBF(BinaryContext &BC,
                          std::vector<BinaryFunction *> &BFs) {
  return getBF(BC, BFs, GolangPass::getLastBFName());
}

uint64_t readEndianVal(DataExtractor &DE, uint64_t *Offset, uint16_t Size);

uint32_t readVarint(DataExtractor &DE, uint64_t *Offset);

int32_t readVarintPair(DataExtractor &DE, uint64_t *MapOffset, int32_t &ValSum,
                       uint32_t &OffsetSum);

template <typename T> static T writeEndian(BinaryContext &BC, T Val) {
  T Ret;
  SmallVector<char, sizeof(T)> Tmp;
  raw_svector_ostream OS(Tmp);
  enum llvm::endianness Endian = llvm::endianness::big;
  if (BC.AsmInfo->isLittleEndian())
    Endian = llvm::endianness::little;

  struct support::endian::Writer Writer(OS, Endian);
  Writer.write<T>(Val);
  memcpy(&Ret, OS.str().data(), sizeof(T));
  return Ret;
}

void writeEndianVal(BinaryContext &BC, uint64_t Val, uint16_t Size,
                    uint8_t **Res) {
  switch (Size) {
  case 8: {
    uint64_t Endian = writeEndian<uint64_t>(BC, Val);
    **(uint64_t **)Res = Endian;
    break;
  }

  case 4: {
    uint32_t Endian = writeEndian<uint32_t>(BC, (uint32_t)Val);
    **(uint32_t **)Res = Endian;
    break;
  }

  case 2: {
    uint16_t Endian = writeEndian<uint16_t>(BC, (uint16_t)Val);
    **(uint16_t **)Res = Endian;
    break;
  }

  case 1: {
    **Res = (uint8_t)Val;
    break;
  }

  default:
    llvm_unreachable("Wrong type size");
    exit(1);
  }

  *Res += Size;
}

inline void writeEndianPointer(BinaryContext &BC, uint64_t Val, uint8_t **Res) {
  return writeEndianVal(BC, Val, BC.AsmInfo->getCodePointerSize(), Res);
}

std::string getVarintName(uint32_t Index, bool IsNext);

void addVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                         int32_t Value, bool IsNext, unsigned AllocId);

bool hasVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                         bool IsNext);

int32_t getVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                            bool IsNext);

std::string getFuncdataName(uint32_t Findex, uint32_t Size);

std::string getFuncdataSizeName(uint32_t Findex);

void addFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex,
                           int32_t Value, unsigned AllocId);

bool hasFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex);

uint32_t getFuncdataSizeAnnotation(BinaryContext &BC, MCInst &II,
                                   uint32_t Findex);

int32_t getFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex,
                              uint32_t Index);

void AddRelaReloc(BinaryContext &BC, MCSymbol *Symbol, BinarySection *Section,
                  uint64_t Offset, uint64_t Addend = 0);

static std::vector<BinaryFunction *>
getSortedGolangFunctions(BinaryContext &BC) {
  std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
  BFs.erase(std::remove_if(BFs.begin(), BFs.end(),
                           [](BinaryFunction *BF) {
                             return !BF->isGolang() || BF->isFolded();
                           }),
            BFs.end());
  return BFs;
}

Pclntab::~Pclntab() {}

int Pclntab::readHeader(BinaryContext &BC, const uint64_t PclntabHeaderAddr) {
  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabHeaderAddr);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  uint64_t Offset = PclntabHeaderAddr - Section->getAddress();
  setPclntabHeaderOffset(Offset);
  const uint8_t *SectionBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  __readHeaderRaw(BC, SectionBuffer, IsLittleEndian);

  if (!checkMagic()) {
    errs() << "BOLT-ERROR: pclntab bad magic!\n";
    return -1;
  }

  if (getPsize() != BC.AsmInfo->getCodePointerSize()) {
    outs() << "BOLT-ERROR: pclntab bad pointer size!\n";
    return -1;
  }

  return 0;
}

int Pclntab::writeHeader(BinaryContext &BC, uint8_t *Pclntab) {
  setNewHeaderOffsets();
  __writeHeader(BC, Pclntab);
  return 0;
}

Module::~Module() {}

int Module::read(const BinaryContext &BC) {
  // NOTE The local.moduledata are used in plugins.
  // The firstmoduledata symbol still could be found there
  // but it will point in BSS section */
  const BinaryData *Module = BC.getFirstBinaryDataByName("local.moduledata");
  if (!Module)
    Module = BC.getFirstBinaryDataByName("runtime.firstmoduledata");

  if (!Module) {
    errs() << "BOLT-ERROR: failed to get firstmoduledata symbol!\n";
    return -1;
  }

  const BinarySection *Section = &Module->getSection();
  const uint8_t *ModuleBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  uint64_t Offset = Module->getAddress() - Section->getAddress();
  uint64_t *ModuleArr = getModule();
  for (size_t I = 0; I < getModuleSize() / sizeof(uint64_t); ++I) {
    const uint8_t *ReadPtr = ModuleBuffer + Offset;
    ModuleArr[I] = readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(uint64_t));
    Offset = ReadPtr - ModuleBuffer;
  }

  return 0;
}

GoFunc::~GoFunc() {}

std::unique_ptr<struct Module> createGoModule() {
  if (opts::GolangPass == opts::GV_1_24)
    return std::make_unique<Module_v1_24_6>();
  if (opts::GolangPass == opts::GV_1_22)
    return std::make_unique<Module_v1_22_1>();
  if (opts::GolangPass == opts::GV_1_20)
    return std::make_unique<Module_v1_20_7>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

std::unique_ptr<class Pclntab> createGoPclntab() {
  if (opts::GolangPass == opts::GV_1_24)
    return std::make_unique<Pclntab_v1_24_6>();
  if (opts::GolangPass == opts::GV_1_22)
    return std::make_unique<Pclntab_v1_22_1>();
  if (opts::GolangPass == opts::GV_1_20)
    return std::make_unique<Pclntab_v1_20_7>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

std::unique_ptr<struct GoFunc> createGoFunc() {
  if (opts::GolangPass == opts::GV_1_24)
    return std::make_unique<GoFunc_v1_24_6>();
  if (opts::GolangPass == opts::GV_1_22)
    return std::make_unique<GoFunc_v1_22_1>();
  if (opts::GolangPass == opts::GV_1_20)
    return std::make_unique<GoFunc_v1_20_7>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

struct StackVal {
  uint32_t Size;
  int32_t OldVal;
  int32_t Val;
};

using InstBias = std::map<uint32_t, struct StackVal>;

int GolangPass::getNextMCinstVal(FunctionLayout::block_iterator BBIt,
                                 uint64_t I, const uint32_t Index, int32_t &Val,
                                 uint64_t *NextOffset) {
  BinaryFunction *BF = (*BBIt)->getFunction();
  BinaryContext &BC = BF->getBinaryContext();
  // We're interating in value for the next instruction
  auto II = std::next((*BBIt)->begin(), I + 1);
  do {
    if (II == (*BBIt)->end()) {
      BBIt = std::next(BBIt);
      if (BBIt == BF->getLayout().block_end()) {
        // Last Instruction
        return -1;
      }

      II = (*BBIt)->begin();
    }

    while (II != (*BBIt)->end() && !hasVarintAnnotation(BC, *II, Index)) {
      if (NextOffset)
        *NextOffset += BC.computeInstructionSize(*II);
      II = std::next(II);
    }

  } while (II == (*BBIt)->end());

  Val = getVarintAnnotation(BC, *II, Index);
  return 0;
}

void GolangMetadataRewriter::addMetadataPatch(BinarySection &Section,
                                              uint64_t Offset, uint32_t Value) {
  // typelinksPass invokes typePass on a thread pool and BinarySection's
  // pending-relocation vector is a plain std::vector: serialize appends.
  // All workers are joined long before flushPendingRelocations runs.
  static std::mutex PatchMutex;
  std::lock_guard<std::mutex> Lock(PatchMutex);
  Section.addPendingRelocation(Relocation{Offset, /*Symbol=*/nullptr,
                                          Relocation::getAbs32(), Value,
                                          /*Value=*/0});
}

void GolangMetadataRewriter::inlTreePass(BinaryFunction *BF,
                                         struct GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const unsigned InlIndex = GoFunc->getFuncdataInlTreeIndex();
  if (GoFunc->getNfuncdata() <= InlIndex)
    return;

  const uint64_t Address = GoFunc->getFuncdata(InlIndex);
  if (!Address)
    return;

  ErrorOr<BinarySection &> FuncdataSection = BC.getSectionForAddress(Address);
  if (!FuncdataSection) {
    errs() << "BOLT-ERROR: failed to get section for inline 0x"
           << Twine::utohexstr(Address) << "\n";
  }

  const uint8_t *FuncdataBuffer =
      reinterpret_cast<const uint8_t *>(FuncdataSection->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  std::unordered_map<uint32_t, uint32_t> ParentOffset; // Val:newOffset
  for (BinaryBasicBlock *BB : BF->getLayout().blocks()) {
    if (!BB->hasInstructions())
      continue;
    for (MCInst &II : *BB) {
      if (!hasFuncdataAnnotation(BC, II, InlIndex))
        continue;

      uint32_t Size = getFuncdataSizeAnnotation(BC, II, InlIndex);
      for (uint32_t I = 0; I < Size; ++I) {
        int32_t Index = getFuncdataAnnotation(BC, II, InlIndex, I);
        ParentOffset[Index] = getBFInstrOffset(BB, &II);
      }
    }
  }

  for (const auto &KV : ParentOffset) {
    const uint32_t Index = KV.first;
    const uint32_t NewOffset = KV.second;
    uint64_t Offset = Address - FuncdataSection->getAddress();
    Offset += Index * sizeof(InlinedCall_v1_20_7);

    const uint8_t *ReadPtr = FuncdataBuffer + Offset;
    ReadPtr += 4; // FuncID
    ReadPtr += 3; // Unused
    ReadPtr += 4; // NameOff
    const int32_t OldParentPc =
        (int32_t)readEndianValRaw(&ReadPtr, IsLittleEndian, 4);
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: inlTreePass: " << BF->getOneName()
                      << " Index=" << Index << " ParentPc: " << OldParentPc
                      << "->" << NewOffset << "\n";);

    addMetadataPatch(*FuncdataSection,
                     Offset + offsetof(struct InlinedCall_v1_20_7, ParentPc),
                     NewOffset);
    addMetadataPatch(*FuncdataSection,
                     Offset + offsetof(struct InlinedCall_v1_20_7, StartLine),
                     0);
  }
}

void GolangMetadataRewriter::wrapInfoPass(BinaryFunction *BF,
                                          struct GoFunc *GoFunc,
                                          BinaryFunction *FirstBF) {
  BinaryContext &BC = BF->getBinaryContext();
  if (GoFunc->getFuncID() != GoFunc->getFuncIDForWrapper())
    return;

  auto WrapInfoIndex = GoFunc->getFuncdataWrapInfoIndex();
  if (GoFunc->getNfuncdata() <= *WrapInfoIndex)
    return;

  const uint64_t WrapInfoAddress = GoFunc->getFuncdata(*WrapInfoIndex);
  if (!WrapInfoAddress)
    return;

  ErrorOr<BinarySection &> FuncdataSection =
      BC.getSectionForAddress(WrapInfoAddress);
  if (!FuncdataSection)
    return;

  const uint8_t *FuncdataBuffer =
      reinterpret_cast<const uint8_t *>(FuncdataSection->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  uint64_t Offset = (uint64_t)(WrapInfoAddress - FuncdataSection->getAddress());
  const uint8_t *ReadPtr = FuncdataBuffer + Offset;
  uint32_t OldWrappedOff = readEndianValRaw(&ReadPtr, IsLittleEndian, 4);
  const uint64_t WrappedFuncAddress =
      GolangBinaryInfo::getInstance().getGolangFunctionAddress(OldWrappedOff);

  BinaryFunction *WrappedFunc =
      BC.getBinaryFunctionAtAddress(WrappedFuncAddress);
  if (!WrappedFunc)
    return;

  addMetadataPatch(*FuncdataSection, Offset,
                   WrappedFunc->getOutputAddress() -
                       FirstBF->getOutputAddress());
}

int GolangPass::unsafePointPass(BinaryFunction *BF, GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const uint32_t UnsafePointIndex = GoFunc->getPcdataUnsafePointIndex();
  if (!BF->isSimple() || !GoFunc->getNpcdata() ||
      !GoFunc->getPcdata(UnsafePointIndex))
    return 0;

  const int UnsafeVal = GoFunc->getPcdataUnsafePointVal();
  for (BinaryBasicBlock *BB : BF->getLayout().blocks()) {
    for (MCInst &Inst : *BB) {
      bool HasMap = hasVarintAnnotation(BC, Inst, UnsafePointIndex);
      if (HasMap)
        continue;

      // The regular branches are the only exceptions for inserted instructions
      // to be not unsafe point
      if ((BC.MIB->isBranch(Inst)) && !BC.MIB->isIndirectBranch(Inst))
        continue;

      addVarintAnnotation(BC, Inst, UnsafePointIndex, UnsafeVal,
                          /*IsNext*/ false);
    }
  }

  return 0;
}

int GolangPass::pcspInstPass(BinaryFunction *BF, GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const uint32_t PcspIndex = GoFunc->getPcspIndex();
  int32_t Val = 0;
  std::unordered_map<const MCSymbol *, int32_t> SymVal;

  // Add pcsp annotation for inserted instructions.
  // We assume inserted instructions won't change the sp value.
  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    uint64_t Offset = 0;
    const MCSymbol *BBlable = BB->getLabel();
    bool HasSecondaryEntryPoint =
        BB->isEntryPoint() ? BBIt != BF->getLayout().block_begin() : false;

    for (MCInst &Inst : *BB) {
      bool HasMap = hasVarintAnnotation(BC, Inst, PcspIndex);

      if (!HasMap) {
        // Get pcsp for the first instruction in inserted BB
        if (!Offset && HasSecondaryEntryPoint) {
          assert(SymVal.find(BBlable) != SymVal.end() &&
                 "Failed to get pcsp value");
          Val = SymVal[BBlable];
        }

        // Map pcsp for inserted instructions
        addVarintAnnotation(BC, Inst, PcspIndex, Val, /*IsNext*/ false);
      }

      Val = getVarintAnnotation(BC, Inst, PcspIndex);
      if (BC.MIB->isCall(Inst) || BC.MIB->isBranch(Inst)) {
        const MCSymbol *TgtSymbol = BC.MIB->getTargetSymbol(Inst);
        if (TgtSymbol)
          SymVal[TgtSymbol] = Val;
      }
      Offset += BC.computeInstructionSize(Inst);
    }
  }
  return 0;
}

int GolangPass::processPcdata(BinaryContext &BC, BinaryFunction *BF,
                              GoFunc *GoFunc, uint8_t *SectionData,
                              uint8_t **DataFuncOffset) {
  unsafePointPass(BF, GoFunc);
  pcspInstPass(BF, GoFunc);

  GoFunc->fixNpcdata();

  return 0;
}

int GolangPass::pclntabPass(BinaryContext &BC) {
  const uint64_t PcHeaderAddress = getPcHeaderAddr();
  if (!PcHeaderAddress) {
    errs() << "BOLT-ERROR: pclntab address is zero!\n";
    return -1;
  }

  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PcHeaderAddress);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  const unsigned SectionFlags = BinarySection::getFlags(/*IsReadOnly=*/false,
                                                        /*IsText=*/false,
                                                        /*IsAllocatable=*/true);
  BinarySection *OutputSection =
      &BC.registerOrUpdateSection(".pclntab", ELF::SHT_PROGBITS, SectionFlags,
                                  nullptr, ~0ULL, sizeof(uint64_t));

  if (auto It = BC.EndSymbols.find("runtime.epclntab");
      It != BC.EndSymbols.end()) {
    It->second = OutputSection;
  }

  const uint64_t SectionMaxSize =
      alignTo(PclntabSym->getSize(), BC.RegularPageSize) * 8;
  uint8_t *const SectionData = new uint8_t[SectionMaxSize];
  uint8_t *const OldSectionData = Section->getData();
  if (!SectionData) {
    errs() << "BOLT-ERROR: failed to allocate new .pclntab section\n";
    return -1;
  }

  uint64_t SectionEndAddr = Section->getEndAddress();
  if (PcHeaderAddress + PclntabSym->getSize() != SectionEndAddr) {
    opts::GolangRemoveOldPclntab = false;
    outs() << "BOLT-INFO: can not remove old pclntab. "
              "Disable --golang-remove-old-pclntab.\n";
  }

  static std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
  BinaryFunction *FirstBF = getFirstBF(BC, BFs);
  assert(FirstBF && "First function not found");
  BinaryFunction *LastBF = getLastBF(BC, BFs);
  assert(LastBF && "Last function not found");
  const size_t BFCount = getSortedGolangFunctions(BC).size();

  MCSymbol *BeginSym = FirstBF->getSymbol();
  MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
  const uint64_t OffsetOfTextStartInPcHeader = 8 + 2 * getPsize();
  const uint64_t PcHeaderOffset = PcHeaderAddress - Section->getAddress();

  getModule()->setPclntabSize(SectionMaxSize);
  getModule()->setFtabSize(BFCount);

  RemoveRelaReloc(BC, Section, PcHeaderOffset + OffsetOfTextStartInPcHeader);
  AddRelaReloc(BC, BeginSym, OutputSection, OffsetOfTextStartInPcHeader, 0);

  OutputSection->addRelocation(0, ZeroSym, Relocation::getAbs(getPsize()));
  OutputSection->updateContents(SectionData, SectionMaxSize);
  OutputSection->setIsFinalized();
  GolangBinaryInfo::getInstance().setPclntabSection(OutputSection);

  PclntabSym->setOutputSize(SectionMaxSize);
  PclntabSym->setOutputLocation(*OutputSection, 0);

  if (!opts::Instrument && opts::GolangRemoveOldPclntab)
    Section->updateContents(OldSectionData,
                            Section->getSize() - PclntabSym->getSize());

  return 0;
}

// Prepare .findfunctab section in output binary
// It should be created in advance with known size, so it will be filled with
// actual content later in GolangMetadataRewriter::findFuncTabPass executed
// after emitAndLink
int GolangPass::findFuncTabPass(BinaryContext &BC) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangPass::findFuncTabPass\n");
  uint64_t FindFuncTab = getModule()->getFindfunctab();
  if (!FindFuncTab) {
    errs() << "BOLT-ERROR: findfunctab is zero!\n";
    return -1;
  }

  BinaryData *FindfunctabSym = BC.getBinaryDataAtAddress(FindFuncTab);
  if (!FindfunctabSym) {
    errs() << "BOLT-ERROR: failed to get findfunctab symbol!\n";
    return -1;
  }

  const unsigned SectionFlags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                                        /*IsText=*/false,
                                                        /*IsAllocatable=*/true);
  BinarySection *OutputSection = &BC.registerOrUpdateSection(
      ".findfunctab", ELF::SHT_PROGBITS, SectionFlags, nullptr, ~0ULL,
      sizeof(uint64_t)); // 64-bit alignment for function pointer entries

  // NOTE Currently we don't know how much BFs occupy in text section.
  // We will allocate 4 times more then original size using mmap.
  const uint64_t SectionMaxSize =
      alignTo(FindfunctabSym->getSize(), BC.RegularPageSize) * 4;
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangPass::findFuncTabPass: "
                    << "Original findfunctab size=" << FindfunctabSym->getSize()
                    << ", allocating SectionMaxSize=" << SectionMaxSize << " (~"
                    << (SectionMaxSize / 20) << " buckets)\n");
  uint8_t *const SectionData = new uint8_t[SectionMaxSize];
  if (!SectionData) {
    errs() << "BOLT-ERROR: failed to allocate new .findfunctab section\n";
    return -1;
  }
  memset(SectionData, 0, SectionMaxSize);
  OutputSection->updateContents(SectionData, SectionMaxSize);
  FindfunctabSym->setOutputSize(SectionMaxSize);
  FindfunctabSym->setOutputLocation(*OutputSection, 0);

  MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
  OutputSection->addRelocation(0, ZeroSym,
                               Relocation::getAbs(sizeof(uint32_t)));
  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangPass::findFuncTabPass: "
             << "Placeholder section created, waiting for Phase2 fill\n");

  OutputSection->setIsFinalized();
  GolangBinaryInfo::getInstance().setFindfunctabSection(OutputSection);
  return 0;
}

// Fill .findfunctab after emitAndLink with actual content according to known
// Go functions addresses/offsets. This algorithm mirrors the Go compiler's
// findfunctab generation in cmd/link/internal/ld/pcln.go.
// See runtime/symtab.go for the lookup algorithm.
int GolangMetadataRewriter::findFuncTabPass(BinaryContext &BC) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::findFuncTabPass\n");

  // Constants matching Go runtime (runtime/symtab.go:584-595)
  constexpr uint32_t SUBBUCKETS = 16;
  constexpr uint32_t SUBBUCKETSIZE = 256;
  constexpr uint32_t BUCKETSIZE = SUBBUCKETS * SUBBUCKETSIZE; // 4096
  constexpr uint32_t NOIDX = 0x7fffffff;

  BinarySection *OutputSection =
      GolangBinaryInfo::getInstance().getFindfunctabSection();

  std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
  BinaryFunction *LastBF = getLastBF(BC, BFs);
  assert(LastBF && "LastBF not found");
  BinaryFunction *FirstBF = getFirstBF(BC, BFs);
  assert(FirstBF && "FirstBF not found");

  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::findFuncTabPass:\n");
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   FirstBF: " << FirstBF->getPrintName()
                    << " input_addr=" << format_hex(FirstBF->getAddress(), 0)
                    << " output_addr="
                    << format_hex(FirstBF->getOutputAddress(), 0) << "\n");
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   LastBF: " << LastBF->getPrintName()
                    << " input_addr=" << format_hex(LastBF->getAddress(), 0)
                    << " output_addr="
                    << format_hex(LastBF->getOutputAddress(), 0)
                    << " size=" << LastBF->getOutputSize() << "\n");

  uint64_t MinPC = FirstBF->getOutputAddress();
  uint64_t MaxPC = LastBF->getOutputAddress() + LastBF->getOutputSize();

  uint32_t NumSubbuckets = (MaxPC - MinPC + SUBBUCKETSIZE - 1) / SUBBUCKETSIZE;
  uint32_t NumBuckets = (MaxPC - MinPC + BUCKETSIZE - 1) / BUCKETSIZE;

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   MinPC=" << format_hex(MinPC, 0)
                    << " MaxPC=" << format_hex(MaxPC, 0) << "\n");
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   Text range=" << (MaxPC - MinPC)
                    << " bytes, NumBuckets=" << NumBuckets << ", SectionSize="
                    << (NumBuckets * (4 + SUBBUCKETS)) << " bytes\n");

  std::vector<uint32_t> Indexes(NumSubbuckets, NOIDX);

  uint32_t FuncIdx = 0;
  uint32_t LastValidIdx = 0;
  for (BinaryFunction *BF : BFs) {
    uint64_t Start = BF->getOutputAddress();
    uint64_t End = Start + BF->getOutputSize();

    if (Start < MinPC || End > MaxPC) {
      continue;
    }

    if (!BF->isGolang()) {
      LLVM_DEBUG(errs() << "BOLT-DEBUG: skipping non-Go function "
                        << BF->getPrintName() << " in Go text range\n");
      continue;
    }
    if (BF->isFolded()) {
      LLVM_DEBUG(errs() << "BOLT-DEBUG: skipping folded function "
                        << BF->getPrintName() << " in Go text range\n");
      continue;
    }

    LLVM_DEBUG(errs() << "BOLT-DEBUG: processing function "
                      << BF->getPrintName() << " [" << format_hex(Start, 0)
                      << ", " << format_hex(End, 0) << ") idx=" << FuncIdx
                      << " subbuckets=" << ((End - Start + 255) / 256) << "\n");

    for (uint64_t PC = Start; PC < End; PC += SUBBUCKETSIZE) {
      if (PC < MinPC || PC >= MaxPC) {
        errs() << "BOLT-ERROR: function " << BF->getPrintName() << " PC 0x"
               << format_hex(PC, 0) << " outside range [0x"
               << format_hex(MinPC, 0) << ", 0x" << format_hex(MaxPC, 0)
               << ")\n";
        return -1;
      }
      uint32_t SubIdx = (PC - MinPC) / SUBBUCKETSIZE;
      if (SubIdx >= Indexes.size()) {
        errs() << "BOLT-ERROR: subbucket index " << SubIdx << " out of bounds ("
               << Indexes.size() << ") for " << BF->getPrintName() << "\n";
        return -1;
      }
      Indexes[SubIdx] = std::min(Indexes[SubIdx], FuncIdx);
    }

    if (End > Start) {
      uint64_t LastPC = End - 1;
      if (LastPC < MinPC || LastPC >= MaxPC) {
        errs() << "BOLT-ERROR: function " << BF->getPrintName() << " last PC 0x"
               << format_hex(LastPC, 0) << " outside range [0x"
               << format_hex(MinPC, 0) << ", 0x" << format_hex(MaxPC, 0)
               << ")\n";
        return -1;
      }
      uint32_t LastSubIdx = (LastPC - MinPC) / SUBBUCKETSIZE;
      if (LastSubIdx >= Indexes.size()) {
        errs() << "BOLT-ERROR: last subbucket index " << LastSubIdx
               << " out of bounds (" << Indexes.size() << ") for "
               << BF->getPrintName() << "\n";
        return -1;
      }
      Indexes[LastSubIdx] = std::min(Indexes[LastSubIdx], FuncIdx);
    }

    FuncIdx++;
  }

  LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: processed " << FuncIdx
                    << " functions, "
                    << "NumSubbuckets=" << NumSubbuckets
                    << ", NumBuckets=" << NumBuckets << "\n");

  // Fill gaps in the findfunctab table caused by alignment padding between
  // functions or skipped non-Go/folded functions. Any PC in a gap falls back
  // to the nearest preceding function index, matching Go runtime behavior.
  for (uint32_t I = 0; I < Indexes.size(); ++I) {
    if (Indexes[I] != NOIDX) {
      LastValidIdx = Indexes[I];
    } else {
      Indexes[I] = LastValidIdx;
    }
  }

  const uint64_t SectionSize = NumBuckets * (4 + SUBBUCKETS);
  LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: SectionSize=" << SectionSize
                    << "\n");
  uint8_t *SectionData = new uint8_t[SectionSize];
  uint8_t *Data = SectionData;

  for (uint32_t B = 0; B < NumBuckets; ++B) {
    uint32_t BucketBaseIdx = B * SUBBUCKETS;
    if (BucketBaseIdx >= Indexes.size()) {
      errs() << "BOLT-ERROR: bucket " << B << " base index out of range\n";
      delete[] SectionData;
      return -1;
    }
    LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: writing bucket " << B << "/"
                      << NumBuckets << " base=" << Indexes[BucketBaseIdx]
                      << "\n");
    writeEndianVal(BC, Indexes[BucketBaseIdx], 4, &Data);

    for (uint32_t S = 0; S < SUBBUCKETS; ++S) {
      uint32_t SubIdx = B * SUBBUCKETS + S;
      if (SubIdx >= Indexes.size()) {
        LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: bucket " << B
                          << " subbucket " << S
                          << " out of range, writing 0\n");
        *Data++ = 0;
        continue;
      }
      uint32_t Idx = Indexes[SubIdx];
      uint32_t Offset = Idx - Indexes[BucketBaseIdx];

      if (Offset >= 256) {
        errs() << "BOLT-ERROR: too many functions in findfunc bucket! " << B
               << "/" << NumBuckets << " " << S << " " << Offset << "\n";
        delete[] SectionData;
        return -1;
      }
      *Data++ = static_cast<uint8_t>(Offset);
    }
  }

  LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: Data-SectionData="
                    << (Data - SectionData) << " SectionSize=" << SectionSize
                    << "\n");
  if ((uint64_t)(Data - SectionData) != SectionSize) {
    errs() << "BOLT-ERROR: Size mismatch: wrote " << (Data - SectionData)
           << " expected " << SectionSize << "\n";
    delete[] SectionData;
    return -1;
  }

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   Bytes written: " << (Data - SectionData)
                    << " (expected " << SectionSize << ")\n");

  LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: calling updateContents\n");
  OutputSection->updateContents(SectionData, SectionSize);
  LLVM_DEBUG(errs() << "BOLT-DEBUG: findFuncTab: calling setIsFinalized\n");
  OutputSection->setIsFinalized();

  uint64_t FindFuncTab =
      GolangBinaryInfo::getInstance().getModule()->getFindfunctab();
  BinaryData *FindfunctabSym = BC.getBinaryDataAtAddress(FindFuncTab);
  if (FindfunctabSym)
    FindfunctabSym->setOutputSize(SectionSize);

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::findFuncTabPass: "
                    << "Section updated with " << SectionSize << " bytes\n");
  return 0;
}

namespace {
constexpr size_t TypeBatchSize = 100;
} // anonymous namespace

int GolangMetadataRewriter::typePass(BinaryContext &BC, uint64_t TypeAddr,
                                     TypeProcessingState &State) {
  llvm::sys::ScopedReader Lock(State.VisitedMutex);
  uint64_t SectionAddr;

  ErrorOr<BinarySection &> Section = BC.getSectionForAddress(TypeAddr);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get section for type 0x"
           << Twine::utohexstr(TypeAddr) << "\n";
    return -1;
  }

  SectionAddr = Section->getAddress();
  const uint8_t *const SectionData = Section->getData();
  const uint64_t SectionSize = Section->getSize();
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();
  const uint8_t *Cur = SectionData + (TypeAddr - SectionAddr);
  assert(Cur < SectionData + SectionSize && "Invalid offset");

  const uint8_t Psize = GolangBinaryInfo::getInstance().getPsize();

  struct Type {
    uint64_t Size;
    uint64_t Ptrdata;
    uint32_t Hash;
    uint8_t Tflag;
    uint8_t Align;
    uint8_t Fieldalign;
    uint8_t Kind;
    uint64_t CompareFunc;
    uint64_t Gcdata;
    int32_t NameOff;
    int32_t PtrToThis;
  } Type;

  Type.Size = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
  Type.Ptrdata = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
  Type.Hash = readEndianValRaw(&Cur, IsLittleEndian, 4);
  Type.Tflag = readEndianValRaw(&Cur, IsLittleEndian, 1);
  Type.Align = readEndianValRaw(&Cur, IsLittleEndian, 1);
  Type.Fieldalign = readEndianValRaw(&Cur, IsLittleEndian, 1);
  Type.Kind = readEndianValRaw(&Cur, IsLittleEndian, 1);
  Type.CompareFunc = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
  Type.Gcdata = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
  Type.NameOff = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);
  Type.PtrToThis = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);

  if (!(Type.Tflag & UNCOMMON_FLAG))
    return 0;

  uint8_t Kind = Type.Kind & KINDMASK;
  assert(Kind < LastKind && "Wrong kind type");
  assert(Cur < SectionData + SectionSize && "Wrong offset");

  if ((Kind == Ptr) || (Kind == Slice)) {
    uint64_t Address = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Address)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Address);
    }
  } else if (Kind == Struct) {
    struct {
      uint64_t Bytes;
      uint64_t Type;
      uint64_t OffsetAnon;
    } Structfield;

    Cur += Psize;
    uint64_t StructfieldAddress =
        readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    uint64_t Size = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    Cur += Psize;

    assert(Section->containsAddress(StructfieldAddress) &&
           "Wrong StructfieldAddress");
    const uint8_t *StructfieldCur =
        SectionData + (StructfieldAddress - SectionAddr);
    while (Size--) {
      Structfield.Bytes =
          readEndianPointerRaw(&StructfieldCur, IsLittleEndian, Psize);
      Structfield.Type =
          readEndianPointerRaw(&StructfieldCur, IsLittleEndian, Psize);
      Structfield.OffsetAnon =
          readEndianPointerRaw(&StructfieldCur, IsLittleEndian, Psize);
      if (!State.VisitedTypes.count(Structfield.Type)) {
        std::lock_guard<std::mutex> Lock(State.QueueMutex);
        State.DiscoveredTypes.push_back(Structfield.Type);
      }
    }
  } else if (Kind == Interface) {
    struct {
      int32_t Name;
      int32_t Ityp;
    } Imethod;

    Cur += Psize;
    uint64_t MhdrAddress = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    uint64_t Size = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    Cur += Psize;

    assert(Section->containsAddress(MhdrAddress) && "Wrong MhdrAddress");
    const uint8_t *MhdrCur = SectionData + (MhdrAddress - SectionAddr);
    while (Size--) {
      Imethod.Name = (int32_t)readEndianValRaw(&MhdrCur, IsLittleEndian, 4);
      Imethod.Ityp = (int32_t)readEndianValRaw(&MhdrCur, IsLittleEndian, 4);
      uint64_t TypeAddr =
          GolangBinaryInfo::getInstance().getModule()->getTypes() +
          Imethod.Ityp;
      if (!State.VisitedTypes.count(TypeAddr)) {
        std::lock_guard<std::mutex> Lock(State.QueueMutex);
        State.DiscoveredTypes.push_back(TypeAddr);
      }
    }
  } else if (Kind == Func) {
    Cur += 2 * sizeof(uint16_t);
    Cur = SectionData + alignTo(Cur - SectionData, Psize);
  } else if (Kind == Array) {
    uint64_t Addr;
    Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    Cur += Psize;
  } else if (Kind == Chan) {
    uint64_t Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    Cur += Psize;
  } else if (Kind == Map) {
    uint64_t Addr;
    Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    Addr = readEndianPointerRaw(&Cur, IsLittleEndian, Psize);
    if (!State.VisitedTypes.count(Addr)) {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      State.DiscoveredTypes.push_back(Addr);
    }
    if (opts::GolangPass >= opts::GV_1_24 &&
        GolangBinaryInfo::getInstance().hasSwissMap()) {
      Cur += 5 * Psize;
    } else {
      Cur += 2 * Psize;
    }
  }

  assert(static_cast<uint64_t>(Cur - SectionData) ==
             alignTo(Cur - SectionData, Psize) &&
         "Wrong alignment");
  assert(Cur < SectionData + SectionSize && "Invalid Offset");

  uint64_t UncommonOffset = static_cast<uint64_t>(Cur - SectionData);

  struct {
    int32_t Pkgpath;
    uint16_t Mcount;
    uint16_t Xcount;
    uint32_t Moff;
    uint32_t Unused2;
  } Uncommontype;

  Uncommontype.Pkgpath = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);
  Uncommontype.Mcount = readEndianValRaw(&Cur, IsLittleEndian, 2);
  Uncommontype.Xcount = readEndianValRaw(&Cur, IsLittleEndian, 2);
  Uncommontype.Moff = readEndianValRaw(&Cur, IsLittleEndian, 4);
  Uncommontype.Unused2 = readEndianValRaw(&Cur, IsLittleEndian, 4);

  assert(UncommonOffset + Uncommontype.Moff >=
             static_cast<uint64_t>(Cur - SectionData) &&
         "Wrong Moff");
  Cur = SectionData + UncommonOffset + Uncommontype.Moff;

  const uint64_t TextStartAddr = State.TextStartAddr;

  while (Uncommontype.Mcount--) {
    assert(Cur < SectionData + SectionSize && "Invalid offset");
    struct {
      int32_t NameOff;
      int32_t TypeOff;
      int32_t Ifn;
      int32_t Tfn;
    } Method;

    Method.NameOff = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);
    Method.TypeOff = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);
    uint64_t InputOffset = Cur - SectionData;
    Method.Ifn = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);
    uint64_t TfnInputOffset = Cur - SectionData;
    Method.Tfn = (int32_t)readEndianValRaw(&Cur, IsLittleEndian, 4);

    auto setFn = [&](int32_t Value, uint64_t FnInputOffset) {
      if (Value == -1)
        return;

      uint64_t Addr =
          GolangBinaryInfo::getInstance().getGolangFunctionAddress(Value);
      BinaryFunction *Fn = BC.getBinaryFunctionAtAddress(Addr);
      if (!Fn) {
        errs() << "BOLT-ERROR: failed to get Ifn or Tfn!\n";
        exit(1);
      }

      uint32_t RelativeOffset =
          static_cast<uint32_t>(Fn->getOutputAddress() - TextStartAddr);

      addMetadataPatch(*Section, FnInputOffset, RelativeOffset);
    };

    setFn(Method.Ifn, InputOffset);
    setFn(Method.Tfn, TfnInputOffset);
  }

  return 0;
}

int GolangMetadataRewriter::typelinksPass(BinaryContext &BC) {
  uint64_t Types = GolangBinaryInfo::getInstance().getModule()->getTypes();
  if (!Types) {
    errs() << "BOLT-ERROR: types address is zero!\n";
    return -1;
  }

  uint64_t Etypes = GolangBinaryInfo::getInstance().getModule()->getEtypes();
  assert(Types < Etypes && "Wrong Etypes");
  const GoArray &TypeLinks =
      GolangBinaryInfo::getInstance().getModule()->getTypelinks();
  uint64_t TypelinkAddr = TypeLinks.getAddress();
  uint64_t TypelinkCount = TypeLinks.getCount();
  if (!TypelinkAddr) {
    errs() << "BOLT-ERROR: typelink address is zero!\n";
    return -1;
  }

  ErrorOr<BinarySection &> Section = BC.getSectionForAddress(TypelinkAddr);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get typelink section!\n";
    return -1;
  }

  const uint8_t *const SectionData = Section->getData();
  const uint64_t SectionSize = Section->getSize();
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();
  const uint8_t *Cur = SectionData + (TypelinkAddr - Section->getAddress());

  TypeProcessingState State;

  while (TypelinkCount--) {
    assert(Cur < SectionData + SectionSize && "Invalid offset");
    uint64_t Type = Types + readEndianValRaw(&Cur, IsLittleEndian, 4);
    assert(Type < Etypes && "Wrong type offset");
    State.DiscoveredTypes.push_back(Type);
  }

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: initial "
                    << State.DiscoveredTypes.size() << " types\n");

  // Compute the loop-invariant text start address once; typePass used to
  // recompute it (full function sort + linear regex scan) for every
  // method-bearing type, which dominated pass runtime on large binaries.
  {
    std::vector<BinaryFunction *> BFs = getSortedGolangFunctions(BC);
    BinaryFunction *FirstGoFunc = getFirstBF(BC, BFs);
    if (!FirstGoFunc) {
      errs() << "BOLT-ERROR: failed to get first golang function!\n";
      return -1;
    }
    State.TextStartAddr = FirstGoFunc->getOutputAddress();
  }

  if (opts::NoThreads) {
    for (size_t I = 0; I < State.DiscoveredTypes.size() && !State.Error; ++I) {
      uint64_t TypeAddr = State.DiscoveredTypes[I];
      {
        llvm::sys::ScopedWriter Lock(State.VisitedMutex);
        State.VisitedTypes.insert(TypeAddr);
      }
      LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: processing type 0x"
                        << Twine::utohexstr(TypeAddr)
                        << " (single-threaded)\n");
      if (int Ret = typePass(BC, TypeAddr, State); Ret < 0)
        return Ret;
    }
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: finished, total types: "
                      << State.DiscoveredTypes.size()
                      << ", visited: " << State.VisitedTypes.size() << "\n");
    return State.Error ? -1 : 0;
  }

  ThreadPoolInterface &Pool = ParallelUtilities::getThreadPool();

  auto processBatch = [&](size_t BatchStart, size_t BatchEnd) {
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: worker processing types ["
                      << BatchStart << ", " << BatchEnd << ")\n");
    for (size_t I = BatchStart; I < BatchEnd && !State.Error; ++I) {
      // DiscoveredTypes may grow (and reallocate) concurrently via
      // push_back from other workers; read the element under QueueMutex.
      uint64_t TypeAddr;
      {
        std::lock_guard<std::mutex> Lock(State.QueueMutex);
        TypeAddr = State.DiscoveredTypes[I];
      }
      {
        llvm::sys::ScopedWriter Lock(State.VisitedMutex);
        if (State.VisitedTypes.count(TypeAddr))
          continue;
        State.VisitedTypes.insert(TypeAddr);
      }
      LLVM_DEBUG(
          dbgs() << "BOLT-DEBUG: typelinksPass: worker processing type 0x"
                 << Twine::utohexstr(TypeAddr) << "\n");
      if (int Ret = typePass(BC, TypeAddr, State); Ret < 0) {
        State.Error = true;
        break;
      }
    }
    {
      std::lock_guard<std::mutex> Lock(State.QueueMutex);
      --State.ActiveWorkers;
      State.QueueCondVar.notify_all();
    }
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: worker finished ["
                      << BatchStart << ", " << BatchEnd << ")\n");
  };

  size_t BatchStart, BatchEnd;
  while (true) {
    {
      std::unique_lock<std::mutex> Lock(State.QueueMutex);

      State.QueueCondVar.wait(Lock, [&]() {
        return State.Error || State.NextIndex < State.DiscoveredTypes.size() ||
               State.ActiveWorkers == 0;
      });

      if (State.Error)
        break;

      if (State.NextIndex >= State.DiscoveredTypes.size() &&
          State.ActiveWorkers == 0)
        break;

      if (State.NextIndex >= State.DiscoveredTypes.size()) {
        std::this_thread::yield();
        continue;
      }

      BatchStart = State.NextIndex;
      BatchEnd =
          std::min(BatchStart + TypeBatchSize, State.DiscoveredTypes.size());
      State.NextIndex = BatchEnd;
      ++State.ActiveWorkers;
    }

    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: dispatching batch ["
                      << BatchStart << ", " << BatchEnd << ")\n");
    Pool.async(processBatch, BatchStart, BatchEnd);
  }

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: waiting for workers\n");
  Pool.wait();
  assert(State.NextIndex >= State.DiscoveredTypes.size() &&
         "Not all discovered types were processed");
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: typelinksPass: finished, total types: "
                    << State.DiscoveredTypes.size()
                    << ", visited: " << State.VisitedTypes.size() << "\n");

  return State.Error ? -1 : 0;
}

// .pclntab section layout:
// +0x00: pcHeader       PcHeader    see PcHeader binary layout in go_v1_20.h
// +pcHeaderSize: ftab    functab[]   array of functab entries (BFCount + 1)
//                                 Each functab: {int32 Address, int32 Offset}
//                                 First BFCount entries map functions
//                                 Last entry contains maxpc
// +pclntable: pclntable _func[]     array of _func entries (one per function)
//                                 For each function in .pclntab:
//                                   - _func struct (see go_v1_20.h for fields)
//                                   - npcdata array (Npcdata * 4 bytes)
//                                   - funcdata array (Nfuncdata * 4 bytes)
//                                     NOTE: funcdata entries are offsets to:
//                                           - inline tree data
//                                           (funcdata[InlTreeIndex])
//                                           - wrapInfo data
//                                           (funcdata[WrapInfoIndex])
//                                           - other metadata in .rodata section
//                                   - pcdata values (varint-encoded, at
//                                   DataFuncOffset)
//                                   - inline tree data (at FuncPartWrite after
//                                   GoFunc->write())
//                                   - wrapInfo (4 bytes, written by
//                                   wrapInfoPass)
//
//                                 Layout order per function:
//                                   FuncOffset:         _func struct + npcdata
//                                   + funcdata DataFuncOffset:     pcdata
//                                   values (written by processPcdata)
//                                   FuncPartWrite:      inline tree data
//                                   (written by inlTreePass) DataFuncOffset:
//                                   wrapInfo (written by wrapInfoPass) Then
//                                   aligned to Psize for next function

int GolangMetadataRewriter::pclntabPass(BinaryContext &BC) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::pclntabPass\n");

  BinarySection *OutputSection =
      GolangBinaryInfo::getInstance().getPclntabSection();
  Pclntab *Pclntab = GolangBinaryInfo::getInstance().getPclntab();

  std::vector<BinaryFunction *> BFs = getSortedGolangFunctions(BC);
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: BFs.size()=" << BFs.size() << "\n");
  for (BinaryFunction *BF : BFs) {
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG:   BF: " << BF->getPrintName()
                      << " addr=" << BF->getOutputAddress()
                      << " isGolang=" << BF->isGolang() << "\n");
  }
  static std::vector<BinaryFunction *> AllBFs = BC.getSortedFunctions();
  BinaryFunction *FirstBF = getFirstBF(BC, AllBFs);
  BinaryFunction *LastBF = getLastBF(BC, AllBFs);
  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::pclntabPass: FirstBF="
             << FirstBF->getPrintName() << " LastBF="
             << (LastBF ? LastBF->getPrintName() : "nullptr") << "\n");
  uint64_t TextStart = FirstBF->getOutputAddress();

  const uint64_t PcHeaderAddress =
      GolangBinaryInfo::getInstance().getModule()->getPcHeaderAddr();
  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PcHeaderAddress);
  const uint64_t SectionMaxSize =
      alignTo(PclntabSym->getSize(), BC.RegularPageSize) * 8;
  uint8_t *Data = OutputSection->getOutputData();
  memset(Data, 0, SectionMaxSize);
  uint8_t *SectionData = Data;

  BinarySection *OriginalSection = &PclntabSym->getSection();
  const uint8_t *OriginalData = OriginalSection->getData();
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  Pclntab->setFunctionsCount(BFs.size());
  Pclntab->setTextStart(TextStart);
  Pclntab->writeHeader(BC, SectionData);

  const uint8_t PctabEntrySize = 8;
  const uint8_t FunctabFieldSize = PctabEntrySize / 2;
  uint8_t *FunctabPart = SectionData + Pclntab->getPcHeaderSize();
  uint8_t *FuncPart =
      FunctabPart + BFs.size() * PctabEntrySize + PctabEntrySize;

  for (BinaryFunction *BF : BFs) {
    if (!BF->isGolang())
      continue;

    uint64_t OldTabOffset = BF->getGolangFunctabOffset();
    std::unique_ptr<GoFunc> GoFunc = createGoFunc();
    GoFunc->read(BC, OriginalData, IsLittleEndian, nullptr, &OldTabOffset);
    GoFunc->setModule(GolangBinaryInfo::getInstance().getModule());
    GoFunc->disableMetadata();

    const uint32_t FuncOffset = FuncPart - SectionData;

    uint8_t *DataFuncOffset = FuncPart + GoFunc->getSize(BC);
    DataFuncOffset += GoFunc->getPcdataSize();
    DataFuncOffset += GoFunc->getNfuncdata() * 4;

    processPcdata(BC, BF, GoFunc.get(), SectionData, &DataFuncOffset);

    FuncPart = SectionData +
               alignTo(DataFuncOffset - SectionData + GoFunc->getSize(BC) +
                           GoFunc->getPcdataSize() + GoFunc->getNfuncdata() * 4,
                       Pclntab->getPsize());

    const uint32_t AddrDelta =
        BF->getOutputAddress() - FirstBF->getOutputAddress();
    writeEndianVal(BC, AddrDelta, FunctabFieldSize, &FunctabPart);
    writeEndianVal(BC, FuncOffset, FunctabFieldSize, &FunctabPart);
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: functab: " << BF->getPrintName()
                      << " addrDelta=" << AddrDelta
                      << " funcOffset=" << FuncOffset << "\n");

    wrapInfoPass(BF, GoFunc.get(), FirstBF);
    inlTreePass(BF, GoFunc.get());

    uint8_t *FuncPartWrite = SectionData + FuncOffset;
    GoFunc->write(BF, &FuncPartWrite, SectionData, OutputSection,
                  FirstBF->getSymbol(), FirstBF);
  }

  if (LastBF) {
    const uint32_t LastAddrDelta =
        LastBF->getOutputAddress() - FirstBF->getOutputAddress();
    writeEndianVal(BC, LastAddrDelta, FunctabFieldSize, &FunctabPart);
    writeEndianVal(BC, 0, FunctabFieldSize, &FunctabPart);
  }

  PclntabSym->setOutputSize(SectionMaxSize);
  PclntabSym->setOutputLocation(*OutputSection, 0);
  OutputSection->setIsFinalized();

  return 0;
}

uint64_t GolangMetadataRewriter::getBFInstrOffset(const BinaryBasicBlock *BB,
                                                  const MCInst *Instr) {
  const BinaryFunction *BF = BB->getFunction();
  const BinaryContext &BC = BF->getBinaryContext();

  const uint64_t FuncStartAddr =
      (*BF->getLayout().block_begin())->getOutputAddressRange().first;
  const uint64_t BBStartAddr = BB->getOutputAddressRange().first;

  uint64_t OffsetWithinBB = 0;
  for (const MCInst &II : *BB) {
    if (&II == Instr)
      return (BBStartAddr - FuncStartAddr) + OffsetWithinBB;
    OffsetWithinBB += BC.computeInstructionSize(II);
  }

  llvm_unreachable("Wrong BB or Instr");
  exit(1);
}

void GolangMetadataRewriter::writeVarint(uint8_t *Data, uint64_t *Offset,
                                         uint32_t Val) {
  while (Val >= 0x80) {
    Data[(*Offset)++] = (uint8_t)(Val | 0x80);
    Val >>= 7;
  }
  Data[(*Offset)++] = (uint8_t)Val;
}

void GolangMetadataRewriter::writeVarint(uint8_t **Data, uint32_t Val) {
  uint64_t Offset = 0;
  writeVarint(*Data, &Offset, Val);
  *Data += Offset;
}

void GolangMetadataRewriter::writeVarintPair(
    int32_t Val, int32_t &PrevVal, uint64_t Offset, uint64_t &CurrentOffset,
    bool &IsFirst, uint8_t **DataFuncOffset, const uint8_t Quantum,
    const char *FuncName) {
  int32_t V = Val - PrevVal;
  V = (V < 0) ? (((-V - 1) << 1) | 1) : V << 1;
  assert((V != 0 || IsFirst) && "The value detla could not be zero");
  PrevVal = Val;

  uint8_t *BeforeValWrite = *DataFuncOffset;
  writeVarint(DataFuncOffset, (uint32_t)V);
  uint8_t ValBytes = *DataFuncOffset - BeforeValWrite;

  assert((Offset - CurrentOffset) % Quantum == 0 &&
         "Offset it not multiple of quantum");
  uint32_t CurrentDelta = (Offset - CurrentOffset) / Quantum;
  assert((CurrentDelta || IsFirst) && "The offset delta could not be zero");

  uint8_t *BeforeDeltaWrite = *DataFuncOffset;
  writeVarint(DataFuncOffset, CurrentDelta);
  uint8_t DeltaBytes = *DataFuncOffset - BeforeDeltaWrite;

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::" << FuncName
                    << ":varint_pair: Val=" << Val << " Vencoded=" << V
                    << " ValBytes=" << (int)ValBytes
                    << " Delta=" << CurrentDelta
                    << " DeltaBytes=" << (int)DeltaBytes << "\n");

  CurrentOffset = Offset;
  IsFirst = false;
}

int GolangMetadataRewriter::getNextMCinstVal(
    FunctionLayout::block_iterator BBIt, uint64_t I, const uint32_t Index,
    int32_t &Val, uint64_t *NextOffset) {
  BinaryFunction *BF = (*BBIt)->getFunction();
  BinaryContext &BC = BF->getBinaryContext();
  auto II = std::next((*BBIt)->begin(), I + 1);
  do {
    if (II == (*BBIt)->end()) {
      BBIt = std::next(BBIt);
      if (BBIt == BF->getLayout().block_end()) {
        return -1;
      }
      II = (*BBIt)->begin();
    }
    while (II != (*BBIt)->end() && !hasVarintAnnotation(BC, *II, Index)) {
      if (NextOffset)
        *NextOffset = getBFInstrOffset(*BBIt, &(*II));
      II = std::next(II);
    }
  } while (II == (*BBIt)->end());
  if (NextOffset)
    *NextOffset = getBFInstrOffset(*BBIt, &(*II));
  Val = getVarintAnnotation(BC, *II, Index);
  return 0;
}

uint32_t GolangMetadataRewriter::deferreturnPass(BinaryContext &BC,
                                                 BinaryFunction *Function) {
  for (const BinaryBasicBlock *BB : Function->getLayout().rblocks()) {
    for (auto II = BB->begin(); II != BB->end(); ++II) {
      if (!BC.MIB->hasAnnotation(*II, "IsDefer"))
        continue;
      return getBFInstrOffset(BB, &(*II));
    }
  }
  errs() << "BOLT-ERROR: deferreturn call was not found for " << *Function
         << "\n";
  exit(1);
}

// Re-number _PCDATA_Restart1(2) pcdata entries for given function.
// // After this function - adjacent sequences of instructions in adjacent
// // BBs marked by _PCDATA_Restart1(2) pcdata should have different values.
// // So preemption Golang code will be managed to distinguish them.
void GolangMetadataRewriter::updRestartVarintPass(BinaryContext &BC,
                                                  BinaryFunction *BF,
                                                  GoFunc *GoFunc) {
  bool Replaced = false;
  int32_t PrevVal = -1, Val = -1;
  const uint32_t Index = GoFunc->getPcdataUnsafePointIndex();
  const int32_t RestartVal1 = GoFunc->getPcdataRestart1();
  const int32_t RestartVal2 = GoFunc->getPcdataRestart2();
  int32_t RestartVal = RestartVal1;
  uint32_t Distance = 0;
  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    if (!BB->hasInstructions())
      continue;
    PrevVal = -1;
    for (auto II = BB->begin(); II != BB->end(); ++II) {
      if (!hasVarintAnnotation(BC, *II, Index))
        continue;

      Val = getVarintAnnotation(BC, *II, Index);
      if (Val == PrevVal)
        continue;

      PrevVal = Val;
      while (Val == PrevVal) {
        if (Val == RestartVal1 || Val == RestartVal2) {
          replaceVarintAnnotation(BC, *II, Index, RestartVal, /*IsNext*/ false);
          Replaced = true;
          // Instrumentation instructions are BOLT-specific and not part of
          // the original Go program. They should not count toward the Go
          // runtime's async preemption distance limit since they don't exist
          // when the runtime needs to find a restart point.
          if (!BC.MIB->hasAnnotation(*II, "IsInstrumentation"))
            Distance += BC.computeInstructionSize(*II);
        }

        ++II;
        if (II == BB->end())
          break;

        if (!hasVarintAnnotation(BC, *II, Index)) {
          if (Replaced) {
            Val = GoFunc->getPcdataUnsafePointVal();
            addVarintAnnotation(BC, *II, Index, Val, /*IsNext*/ false);
          }
          break;
        }

        Val = getVarintAnnotation(BC, *II, Index);
      }

      if (Replaced) {
        if (Distance > 20) {
          errs() << "BOLT-ERROR: Golang _PCDATA_Restart1(2) distance ("
                 << Distance << ") exceeds maximum (20) for " << *BF
                 << " function\n";
          exit(1);
        }
        RestartVal = (RestartVal == RestartVal1) ? RestartVal2 : RestartVal1;
        Replaced = false;
        Distance = 0;
      }

      --II;
    }
  }
}

int GolangMetadataRewriter::writeVarintPass(BinaryFunction *BF, GoFunc *GoFunc,
                                            uint8_t **DataFuncOffset,
                                            const uint32_t Index,
                                            const uint8_t Quantum) {
  BinaryContext &BC = BF->getBinaryContext();

  int32_t PrevVal = -1;
  uint64_t CurrentOffset = 0;
  bool IsFirst = true;

  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: writeVarintPass: " << BF->getPrintName()
                    << " Index=" << Index << "\n");

  if (Index == GoFunc->getPcdataUnsafePointIndex()) {
    updRestartVarintPass(BC, BF, GoFunc);
  }

  int32_t NextVal;
  uint64_t NextOffset;

  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    for (uint64_t I = 0; I < BB->size(); ++I) {
      MCInst &Inst = BB->getInstructionAtIndex(I);
      if (!hasVarintAnnotation(BC, Inst, Index))
        continue;

      const int32_t Val = getVarintAnnotation(BC, Inst, Index);

      const int Ret = getNextMCinstVal(BBIt, I, Index, NextVal, &NextOffset);
      if (Ret < 0) {
        uint64_t FuncEndOffset = 0;
        for (const BinaryBasicBlock *BB : BF->getLayout().blocks()) {
          FuncEndOffset += BB->getOutputSize();
        }
        writeVarintPair(Val, PrevVal, FuncEndOffset, CurrentOffset, IsFirst,
                        DataFuncOffset, Quantum, BF->getPrintName().c_str());
        break;
      }

      if (Val != NextVal) {
        writeVarintPair(Val, PrevVal, NextOffset, CurrentOffset, IsFirst,
                        DataFuncOffset, Quantum, BF->getPrintName().c_str());
      }
    }
  }

  **DataFuncOffset = 0;
  (*DataFuncOffset)++;

  return 0;
}

int GolangMetadataRewriter::unsafePointPass(BinaryFunction *BF,
                                            GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const uint32_t UnsafePointIndex = GoFunc->getPcdataUnsafePointIndex();
  if (!BF->isSimple() || !GoFunc->getNpcdata() ||
      !GoFunc->getPcdata(UnsafePointIndex))
    return 0;

  const int UnsafeVal = GoFunc->getPcdataUnsafePointVal();
  for (BinaryBasicBlock *BB : BF->getLayout().blocks()) {
    if (!BB->hasInstructions())
      continue;
    for (MCInst &Inst : *BB) {
      bool HasMap = hasVarintAnnotation(BC, Inst, UnsafePointIndex);
      if (HasMap)
        continue;

      if ((BC.MIB->isBranch(Inst)) && !BC.MIB->isIndirectBranch(Inst))
        continue;

      addVarintAnnotation(BC, Inst, UnsafePointIndex, UnsafeVal,
                          /*IsNext*/ false);
    }
  }

  return 0;
}

int GolangMetadataRewriter::pcspInstPass(BinaryFunction *BF, GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const uint32_t PcspIndex = GoFunc->getPcspIndex();
  int32_t Val = 0;
  std::unordered_map<const MCSymbol *, int32_t> SymVal;

  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    if (!BB->hasInstructions())
      continue;
    uint64_t Offset = 0;
    const MCSymbol *BBlable = BB->getLabel();
    bool HasSecondaryEntryPoint =
        BB->isEntryPoint() ? BBIt != BF->getLayout().block_begin() : false;

    for (MCInst &Inst : *BB) {
      bool HasMap = hasVarintAnnotation(BC, Inst, PcspIndex);

      if (!HasMap) {
        if (!Offset && HasSecondaryEntryPoint) {
          assert(SymVal.find(BBlable) != SymVal.end() &&
                 "Failed to get pcsp value");
          Val = SymVal[BBlable];
        }

        addVarintAnnotation(BC, Inst, PcspIndex, Val, /*IsNext*/ false);
      }

      Val = getVarintAnnotation(BC, Inst, PcspIndex);
      if (BC.MIB->isCall(Inst) || BC.MIB->isBranch(Inst)) {
        const MCSymbol *TgtSymbol = BC.MIB->getTargetSymbol(Inst);
        if (TgtSymbol)
          SymVal[TgtSymbol] = Val;
      }
      Offset += BC.computeInstructionSize(Inst);
    }
  }
  return 0;
}

int GolangMetadataRewriter::processPcdata(BinaryContext &BC, BinaryFunction *BF,
                                          GoFunc *GoFunc, uint8_t *SectionData,
                                          uint8_t **DataFuncOffset) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: processPcdata: " << BF->getPrintName()
                    << " start DataFuncOffset="
                    << (*DataFuncOffset - SectionData) << "\n");
  auto setPcdata = [&](const uint32_t Index) {
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: setPcdata index=" << Index
                      << " npcdata=" << GoFunc->getNpcdata() << "\n");
    if (!GoFunc->getNpcdata()) {
      GoFunc->setPcdata(Index, 0);
      return;
    }

    if (!GoFunc->getPcdata(Index))
      return;

    GoFunc->setPcdata(Index, *DataFuncOffset - SectionData);
    if (writeVarintPass(
            BF, GoFunc, DataFuncOffset, Index,
            GolangBinaryInfo::getInstance().getPclntab()->getQuantum()) < 0) {
      GoFunc->setPcdata(Index, 0);
      return;
    }
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: setPcdata index=" << Index << " offset="
                      << GoFunc->getPcdata(Index) << " final DataFuncOffset="
                      << (*DataFuncOffset - SectionData) << "\n");
  };

  setPcdata(GoFunc->getPcdataUnsafePointIndex());
  setPcdata(GoFunc->getPcdataStackMapIndex());
  setPcdata(GoFunc->getPcdataInlTreeIndex());

  if (auto Index = GoFunc->getPcdataArgLiveIndex())
    setPcdata(*Index);

  GoFunc->fixNpcdata();

  if (GoFunc->getDeferreturnOffset())
    GoFunc->setDeferreturnOffset(deferreturnPass(BC, BF));

  if (GoFunc->getPcspOffset()) {
    GoFunc->setPcspOffset(*DataFuncOffset - SectionData);
    int Ret = writeVarintPass(
        BF, GoFunc, DataFuncOffset, GoFunc->getPcspIndex(),
        GolangBinaryInfo::getInstance().getPclntab()->getQuantum());
    if (Ret < 0)
      return Ret;
  }

  return 0;
}

void GolangPass::annotateInstrumentationInstructions(
    InstructionListType &Instrs, BinaryBasicBlock &BB,
    BinaryBasicBlock::iterator Iter) {
  if (!opts::GolangPass || !BB.getFunction()->isGolang())
    return;

  // PCSP value must be updated for instrumentation snippet
  // Get annotation for Iter and add value for instrumented snippet
  std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
  BinaryContext &BC = BB.getFunction()->getBinaryContext();
  int32_t pcspVal = -1;
  int32_t spDelta = 0;
  int32_t stackMapInd = -1;
  std::string PscpVarintName = getVarintName(GoFunc->getPcspIndex());
  std::string UnsafePointVarintName =
      getVarintName(GoFunc->getPcdataUnsafePointIndex());
  std::string StackMapIndexVarintName =
      getVarintName(GoFunc->getPcdataStackMapIndex());
  std::string StackMapIndexVarintNameNext =
      getVarintName(GoFunc->getPcdataStackMapIndex(), true);

  // VARINT0
  for (MCInst &NewInst : Instrs) {
    BC.MIB->getOrCreateAnnotationAs<int64_t>(NewInst, UnsafePointVarintName) =
        GoFunc->getPcdataUnsafePointVal();
  }

  // VARINT1 & VARINT_NEXT1
  if (BC.MIB->hasAnnotation(*Iter, StackMapIndexVarintName)) {
    stackMapInd =
        BC.MIB->getAnnotationAs<int32_t>(*Iter, StackMapIndexVarintName);
    for (MCInst &NewInst : Instrs) {
      BC.MIB->getOrCreateAnnotationAs<int64_t>(
          NewInst, StackMapIndexVarintName) = stackMapInd;
      BC.MIB->getOrCreateAnnotationAs<int64_t>(
          NewInst, StackMapIndexVarintNameNext) = stackMapInd;
    }
  }

  // VARINT5
  if (BC.MIB->hasAnnotation(*Iter, PscpVarintName)) {
    pcspVal = BC.MIB->getAnnotationAs<int32_t>(*Iter, PscpVarintName);
    for (MCInst &NewInst : Instrs) {
      BC.MIB->getOrCreateAnnotationAs<uint64_t>(NewInst, PscpVarintName) =
          pcspVal + spDelta;
      if (BC.MIB->isStackAdjustment(NewInst)) {
        int32_t Adjustment =
            static_cast<int32_t>(BC.MIB->getStackAdjustment(NewInst));
        spDelta += Adjustment;
      }
    }
  }

  for (MCInst &NewInst : Instrs) {
    BC.MIB->getOrCreateAnnotationAs<bool>(NewInst, "IsInstrumentation") = true;
  }
}

bool GolangPass::hasMorestackBlock(const BinaryFunction &Function) {
  const BinaryContext &BC = Function.getBinaryContext();

  for (const BinaryBasicBlock &BB : Function) {
    for (const MCInst &Inst : BB) {
      if (BC.MIB->isCall(Inst)) {
        if (const MCSymbol *Target = BC.MIB->getTargetSymbol(Inst)) {
          StringRef TargetName = Target->getName();
          if (TargetName.contains("runtime.morestack_noctxt.abi0")) {
            LLVM_DEBUG(dbgs() << "BOLT-GOLANG: Function " << Function
                              << " has morestack block (target: " << TargetName
                              << ")\n");
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool GolangPass::hasIndirectCall(const BinaryFunction &Function) {
  const BinaryContext &BC = Function.getBinaryContext();
  for (const BinaryBasicBlock &BB : Function) {
    for (const MCInst &Inst : BB) {
      if (BC.MIB->isIndirectCall(Inst))
        return true;
    }
  }
  return false;
}

std::optional<uint32_t>
GolangPass::computeGoFunctionFrameSize(const BinaryFunction &Function) {
  const BinaryContext &BC = Function.getBinaryContext();

  if (Function.empty())
    return std::nullopt;

  const BinaryBasicBlock &EntryBB = **Function.getLayout().block_begin();

  LLVM_DEBUG(dbgs() << "BOLT-GOLANG: computeGoFunctionFrameSize for "
                    << Function << " (EntryBB size=" << EntryBB.size()
                    << ")\n");

  const BinaryBasicBlock *FrameSetupBB = EntryBB.getFallthrough();
  if (!FrameSetupBB) {
    LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   No fallthrough block\n");
    return std::nullopt;
  }

  LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   Looking at fallthrough BB (size="
                    << FrameSetupBB->size() << ")\n");

  int MaxAdjustment = 0;
  for (const MCInst &Inst : *FrameSetupBB) {
    if (BC.MIB->isCall(Inst))
      break;

    if (BC.MIB->isStackAdjustment(Inst)) {
      int Adjustment = BC.MIB->getStackAdjustment(Inst);
      int AbsAdjustment = std::abs(Adjustment);
      LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   Found stack adjustment: "
                        << Adjustment << " (abs=" << AbsAdjustment << ")\n");
      if (AbsAdjustment > MaxAdjustment) {
        MaxAdjustment = AbsAdjustment;
      }
    }
  }

  if (MaxAdjustment > 0) {
    LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   Frame size = " << MaxAdjustment
                      << "\n");
    return static_cast<uint32_t>(MaxAdjustment);
  }

  LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   No frame size found\n");
  return std::nullopt;
}

const MCSymbol *
GolangPass::findMorestackBranchTarget(const BinaryFunction &Function) {
  const BinaryContext &BC = Function.getBinaryContext();

  LLVM_DEBUG(dbgs() << "BOLT-GOLANG: findMorestackBranchTarget for " << Function
                    << "\n");

  for (const BinaryBasicBlock &BB : Function) {
    for (const MCInst &Inst : BB) {
      if (BC.MIB->isCall(Inst)) {
        if (const MCSymbol *Target = BC.MIB->getTargetSymbol(Inst)) {
          StringRef TargetName = Target->getName();
          if (TargetName.contains("runtime.morestack_noctxt.abi0")) {
            LLVM_DEBUG(dbgs()
                       << "BOLT-GOLANG:   Found morestack call, BB label: "
                       << BB.getLabel()->getName() << "\n");
            return BB.getLabel();
          }
        }
      }
    }
  }

  LLVM_DEBUG(dbgs() << "BOLT-GOLANG:   Morestack BB not found\n");

  return nullptr;
}

// Determine whether to skip extended stack check insertion for a function.
// Returns true if the function should be skipped (no extended stack check
// inserted).
//
// Extended stack checks are inserted for Go user functions on supported
// architectures (AArch64 and x86_64) to handle indirect calls that may require
// more stack space than the standard Go prologue check can detect.
//
// Functions that are skipped:
// - Non-Go functions
// - Non-AArch64/x86_64 architectures
// - Functions in the "runtime." namespace (runtime handles its own stack
// checking)
bool GolangPass::shouldSkipExtendStack(const BinaryContext &BC,
                                       const BinaryFunction &Function) {
  // Skip non-Go functions
  if (!Function.isGolang())
    return true;

  // Only support AArch64 (original implementation) and x86_64 (this port)
  // Note: AArch64 uses X28 register for g pointer, x86_64 uses R14
  if (!BC.isAArch64() && !BC.isX86())
    return true;

  // Skip runtime functions - they have their own stack management
  for (StringRef Name : Function.getNames()) {
    if (Name.contains("runtime."))
      return true;
  }

  return false;
}

// Validate version format (goX.Y.Z)
static bool validateVersionFormat(StringRef VersionStr) {
  if (VersionStr.empty())
    return false;

  if (!VersionStr.starts_with("go"))
    return false;

  const char *FirstDot = VersionStr.data();
  FirstDot = strchr(FirstDot, '.');
  if (!FirstDot || FirstDot == VersionStr.data() + 2)
    return false;

  const char *SecondDot = strchr(FirstDot + 1, '.');
  if (!SecondDot || SecondDot == FirstDot + 1)
    return false;

  for (const char *p = FirstDot + 1; p < SecondDot; p++) {
    if (!isdigit(*p))
      return false;
  }

  // Only validate the version numbers, allow any suffix after
  return true;
}

static int goAnnotationMarkerChecker(BinaryContext &BC) {
  bool checkPassed = true;
  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
      for (MCInst &II : *BB) {
        if (BB->getName().starts_with(".LSplitEdge"))
          continue;
        if (!BC.MIB->hasAnnotation(II, "GoInstMarker") &&
            !BC.MIB->hasAnnotation(II, "IsInstrumentation")) {
          errs() << "BOLT-WARNING: GolangPass Annotation Marker Checker didn't "
                    "find marker in "
                 << BF << '\n';
          BB->dump();
          checkPassed = false;
        }
      }
    }
  };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, skipFunc,
      "goAnnotationMarkerChecker",
      /*ForceSequential*/ true);
  if (!checkPassed)
    return -1;
  return 0;
}

Error GolangPass::runOnFunctions(BinaryContext &BC) {
  int Ret;

#define CALL_STAGE(func)                                                       \
  {                                                                            \
    NamedRegionTimer T("final-" #func, "golang final " #func,                  \
                       GolangTimerGroupName, GolangTimerGroupDesc,             \
                       opts::TimeOpts);                                        \
    Ret = func(BC);                                                            \
  }                                                                            \
  if (Ret < 0) {                                                               \
    errs() << "BOLT-ERROR: golang " << #func << " stage failed!\n";            \
    exit(1);                                                                   \
  }

  if (opts::GolangAnnotationChecker) {
    CALL_STAGE(goAnnotationMarkerChecker);
  }

  CALL_STAGE(pclntabPass);

  CALL_STAGE(findFuncTabPass);

  {
    NamedRegionTimer T("final-module-patch", "golang final module patch",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    Ret = GolangBinaryInfo::getInstance().patch(BC);
  }
  if (Ret < 0) {
    errs() << "BOLT-ERROR: golang module patch stage failed!\n";
    exit(1);
  }

#undef CALL_STAGE
  return Error::success();
}

GolangBinaryInfo GolangBinaryInfo::Instance;

GolangBinaryInfo &GolangBinaryInfo::getInstance() { return Instance; }

void GolangBinaryInfo::initialize(BinaryContext &BC) {
  if (Initialized)
    return;

  {
    NamedRegionTimer T("init-check-version", "golang init check version",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    if (checkGoVersion(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to check golang version!\n";
      exit(1);
    }
  }

  {
    NamedRegionTimer T("init-symbols", "golang init get symbols",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    if (getSymbols(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to get golang-specific symbols!\n";
      exit(1);
    }
  }

  {
    NamedRegionTimer T("init-module-read", "golang init read first module",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    FirstModule = createGoModule();
    if (!FirstModule || FirstModule->read(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to read firstmodule!\n";
      exit(1);
    }
  }

  BC.registerNameAtAddress("funcnametab", FirstModule->getFuncnameTabAddr(), 0,
                           0);

  {
    NamedRegionTimer T("init-pclntab-header", "golang init read pclntab header",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    Pclntab = createGoPclntab();
    if (!Pclntab || Pclntab->readHeader(BC, getPcHeaderAddr()) < 0) {
      errs() << "BOLT-ERROR: Failed to read pclntab!\n";
      exit(1);
    }
  }

  if (Pclntab->getFunctionsCount() != FirstModule->getFtab().getCount() - 1) {
    errs() << "BOLT-ERROR: Wrong symtab size!\n";
    exit(1);
  }

  {
    NamedRegionTimer T("init-textsectmap-read", "golang init read textsectmap",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    textSectMapReadPass(BC);
  }

  {
    NamedRegionTimer T("init-textsectmap", "golang init patch textsectmap",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    textsectmapPass(BC);
  }

  Initialized = true;
}

uint64_t GolangBinaryInfo::getPcHeaderAddr() const {
  return FirstModule->getPcHeaderAddr();
}

int GolangBinaryInfo::getSymbols(BinaryContext &BC) {
  BinaryData *TextSymbol =
      BC.getFirstBinaryDataByName(GolangPass::getFirstBFName());
  if (!TextSymbol) {
    outs() << "BOLT-WARNING: failed to get text start symbol!\n";
    return -1;
  }

  if (opts::GolangPass >= opts::GV_1_24) {
    BinaryFunction *BF =
        BC.getBinaryFunctionByName("internal/runtime/maps.NewMap");
    if (BF) {
      HasSwissMap = true;
    }
  }

  return 0;
}

int GolangBinaryInfo::checkGoVersion(BinaryContext &BC) {
  BinaryData *BuildVersion =
      BC.getFirstBinaryDataByName("runtime.buildVersion");
  if (!BuildVersion) {
    outs() << "BOLT-WARNING: runtime.buildVersion symbol not found\n";
    return -1;
  }

  BinarySection *Section = &BuildVersion->getSection();
  const uint8_t *VersionBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();

  uint64_t GoVersionOffset = BuildVersion->getAddress() - Section->getAddress();
  const uint8_t *ReadPtr = VersionBuffer + GoVersionOffset;
  uint64_t GoVersionAddr = readEndianValRaw(&ReadPtr, IsLittleEndian,
                                            BC.AsmInfo->getCodePointerSize());
  ErrorOr<BinarySection &> GoVersionSection =
      BC.getSectionForAddress(GoVersionAddr);
  if (!GoVersionSection) {
    outs()
        << "BOLT-WARNING: failed to get binary section for go version string\n";
    return -1;
  }

  size_t DropAmount = GoVersionAddr - GoVersionSection->getAddress();
  StringRef BinaryVersion(GoVersionSection->getContents().data() + DropAmount,
                          16);
  if (!validateVersionFormat(BinaryVersion)) {
    outs() << formatv("BOLT-WARNING: invalid version format: {0}\n",
                      BinaryVersion);
    return -1;
  }

  if (opts::GolangPass != opts::GV_AUTO) {
    StringRef ExpectedVersion(GolangPass::GolangStringVer[opts::GolangPass]);

    if (!BinaryVersion.starts_with(ExpectedVersion)) {
      outs() << "BOLT-WARNING: the binary expected version is "
             << ExpectedVersion << " but found: " << BinaryVersion << "\n";
      return 0;
    } else {
      outs() << "BOLT-INFO: golang version exactly matched: " << ExpectedVersion
             << "\n";
    }
  } else {
    for (int I = opts::GV_LATEST; I > opts::GV_FIRST; --I) {
      if (BinaryVersion == GolangPass::GolangStringVer[I]) {
        outs() << "BOLT-INFO: golang version exactly matched: "
               << GolangPass::GolangStringVer[I] << "\n";
        opts::GolangPass = (opts::GolangVersion)I;
        return 0;
      }
    }

    for (int I = opts::GV_LATEST; I > opts::GV_FIRST; --I) {
      StringRef ExpectedVersion(GolangPass::GolangStringVer[I]);

      if (BinaryVersion.starts_with(ExpectedVersion)) {
        outs() << "BOLT-INFO: golang version family matched: "
               << ExpectedVersion << ".x\n";
        outs() << "BOLT-INFO: binary version: " << BinaryVersion
               << ", using family handler: " << ExpectedVersion << "\n";
        opts::GolangPass = (opts::GolangVersion)I;
        return 0;
      }
    }

    outs() << formatv("BOLT-WARNING: unsupported version: {0}\n",
                      BinaryVersion);
    return -1;
  }

  return 0;
}

int GolangBinaryInfo::textSectMapReadPass(BinaryContext &BC) {
  const GoArray &TextSectMap = FirstModule->getTextsectmap();
  uint64_t TextectionmapAddress = TextSectMap.getAddress();
  uint64_t TextectionmapCount = TextSectMap.getCount();
  if (!TextectionmapAddress) {
    return 0;
  }

  if (opts::Verbosity >= 1) {
    outs() << "BOLT-INFO: textSectMapReadPass Textectionmap Address=0x"
           << Twine::utohexstr(TextectionmapAddress) << '\n';
    outs() << "BOLT-INFO: textSectMapReadPass Textectionmap Count="
           << TextectionmapCount << '\n';
  }

  ErrorOr<BinarySection &> Section =
      BC.getSectionForAddress(TextectionmapAddress);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get textsectmaps section!\n";
    return -1;
  }

  const uint8_t *MapBuffer =
      reinterpret_cast<const uint8_t *>(Section->getContents().data());
  const bool IsLittleEndian = BC.AsmInfo->isLittleEndian();
  uint64_t Offset = (uint64_t)(TextectionmapAddress - Section->getAddress());
  for (uint64_t F = 0; F < TextectionmapCount; ++F) {
    const uint8_t *ReadPtr = MapBuffer + Offset;
    uint64_t Vaddr = readEndianValRaw(&ReadPtr, IsLittleEndian,
                                      BC.AsmInfo->getCodePointerSize());
    uint64_t Vend = readEndianValRaw(&ReadPtr, IsLittleEndian,
                                     BC.AsmInfo->getCodePointerSize());
    uint64_t Vbase = readEndianValRaw(&ReadPtr, IsLittleEndian,
                                      BC.AsmInfo->getCodePointerSize());
    Offset = ReadPtr - MapBuffer;
    Textsecmap.push_back(TextsecmapStruct{Vaddr, Vend, Vbase});
  }
  if (opts::Verbosity >= 1) {
    outs() << "BOLT-INFO: textSectMapReadPass\n";
    outs() << "BOLT-INFO: Vaddr \tEnd \tBaseaddr\n";
    for (auto T = Textsecmap.begin(); T != Textsecmap.end(); ++T) {
      TextsecmapStruct TT = *T;
      outs() << "BOLT-INFO: 0x" << Twine::utohexstr(TT.Vaddr) << " \t0x"
             << Twine::utohexstr(TT.End) << " \t0x"
             << Twine::utohexstr(TT.Baseaddr) << '\n';
    }
  }
  return 0;
}

int GolangBinaryInfo::textsectmapPass(BinaryContext &BC) {
  uint64_t TextectionmapAddress = FirstModule->getTextsectmap().getAddress();
  if (!TextectionmapAddress) {
    outs() << "BOLT-INFO: returning from textsectmapPass\n";
    return 0;
  }
  ErrorOr<BinarySection &> Section =
      BC.getSectionForAddress(TextectionmapAddress);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get textsectmaps section!\n";
    return -1;
  }

  BinaryData *EtextSymbol =
      BC.getFirstBinaryDataByName(GolangPass::getLastBFName());
  if (!EtextSymbol) {
    errs() << "BOLT-ERROR: failed to get etext symbol!\n";
    return -1;
  }

  uint64_t Offset = TextectionmapAddress + getPsize() - Section->getAddress();
  Section->addRelocation(Offset, EtextSymbol->getSymbol(),
                         Relocation::getAbs(BC.AsmInfo->getCodePointerSize()));
  MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
  Section->addRelocation(Offset - getPsize(), ZeroSym,
                         Relocation::getAbs(getPsize()));
  return 0;
}

uint64_t GolangBinaryInfo::getGolangFunctionAddress(uint64_t Address) {
  if (Textsecmap.size() > 1) {
    for (auto T = Textsecmap.begin(); T != Textsecmap.end(); ++T) {
      TextsecmapStruct TT = *T;
      auto TNext = std::next(T, 1);
      if ((Address >= TT.Vaddr && Address < TT.End) ||
          (TNext == Textsecmap.end() && Address == TT.End)) {
        return TT.Baseaddr + Address - TT.Vaddr;
      }
    }
    errs() << "BOLT-ERROR: failed to get function address for 0x"
           << Twine::utohexstr(Address) << "\n";
    assert(false);
  }
  return Address + FirstModule->getText();
}

int GolangBinaryInfo::patch(BinaryContext &BC) {
  return FirstModule->patch(BC);
}

GolangMetadataRewriter::GolangMetadataRewriter(StringRef Name,
                                               BinaryContext &BC)
    : MetadataRewriter(Name, BC) {}

Error GolangMetadataRewriter::preCFGInitializer() {
  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::preCFGInitializer\n");
  return Error::success();
}

Error GolangMetadataRewriter::postCFGInitializer() {
  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::postCFGInitializer\n");
  return Error::success();
}

Error GolangMetadataRewriter::postEmitFinalizer() {
  LLVM_DEBUG(
      dbgs() << "BOLT-DEBUG: GolangMetadataRewriter::postEmitFinalizer\n");

  int Ret;

#define CALL_STAGE(func)                                                       \
  {                                                                            \
    NamedRegionTimer T("rewriter-" #func, "golang rewriter " #func,            \
                       GolangTimerGroupName, GolangTimerGroupDesc,             \
                       opts::TimeOpts);                                        \
    Ret = func(BC);                                                            \
  }                                                                            \
  if (Ret < 0) {                                                               \
    errs() << "BOLT-ERROR: golang " << #func << " stage failed!\n";            \
    exit(1);                                                                   \
  }

  {
    NamedRegionTimer T("golang-metadata-rewriter", "golang metadata rewriter",
                       GolangTimerGroupName, GolangTimerGroupDesc,
                       opts::TimeOpts);
    CALL_STAGE(typelinksPass);
    CALL_STAGE(findFuncTabPass);
    CALL_STAGE(pclntabPass);
  }

#undef CALL_STAGE

  return Error::success();
}

std::unique_ptr<MetadataRewriter>
createGolangMetadataRewriter(BinaryContext &BC) {
  return std::make_unique<GolangMetadataRewriter>("golang-metadata-rewriter",
                                                  BC);
}

void GolangPass::assignGoFunctionIndexes(
    std::map<uint64_t, BinaryFunction> &BFs) {
  // | 0: runtime.text                           |
  // | sorted functions                          |
  // | -3: unsorted functions                    |
  // | -2: runtime.etext                         |
  // | -1: injected functions(including startup) |

  for (auto &I : BFs) {
    auto BF = &I.second;
    if (BF->hasRestoredNameRegex(GolangPass::getFirstBFName())) {
      BF->resetIndex();
      BF->setIndex(GO_FIRST_BF_INDEX);
    } else if (BF->hasRestoredNameRegex(GolangPass::getLastBFName())) {
      BF->resetIndex();
      BF->setIndex(GO_LAST_BF_INDEX);
    } else if (!BF->isGolang()) {
      BF->resetIndex();
    } else if (!BF->hasValidIndex()) {
      BF->setIndex(GO_UNUSED_BF_INDEX);
    }
  }
}

} // end namespace bolt
} // end namespace llvm
