//===--------- Passes/Golang/go_v1_20.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_20_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_20_H

#include "go_base.h"

namespace llvm {
namespace bolt {

class Pclntab_v1_20_7 : public Pclntab {
#define PcHeaderFields                                                         \
  F(uint32_t, false, Magic)                                                    \
  F(uint8_t, false, Zero1)                                                     \
  F(uint8_t, false, Zero2)                                                     \
  F(uint8_t, false, MinLC)                                                     \
  F(uint8_t, false, PtrSize)                                                   \
  F(uint64_t, false, Nfuncs)                                                   \
  F(uint64_t, false, Nfiles)                                                   \
  F(uint64_t, true, textStart)                                                 \
  F(uint64_t, true, FuncnameOffset)                                            \
  F(uint64_t, true, CuOffset)                                                  \
  F(uint64_t, true, FiletabOffset)                                             \
  F(uint64_t, true, PctabOffset)                                               \
  F(uint64_t, true, PclnOffset)

  struct PcHeader {
#define F(Type, IsPointerSize, Field) Type Field;
    PcHeaderFields
#undef F
  } Header;

  // PcHeader binary layout (runtime/symtab.go):
  // +0x00: Magic          uint32_t  0xfffffff1 (magic constant, little-endian)
  // +0x04: Zero1          uint8_t   always 0
  // +0x05: Zero2          uint8_t   always 0
  // +0x06: MinLC          uint8_t   instruction alignment (quantum, e.g. 1)
  // +0x07: PtrSize        uint8_t   pointer size (4 or 8 bytes)
  // +0x08: Nfuncs         uint64_t  number of functions
  // +0x10: Nfiles         uint64_t  number of source files
  // +0x18: textStart      uint64_t  address of runtime.text (pointer-sized)
  // +0x20: FuncnameOffset uint64_t  offset to funcnametab (pointer-sized)
  // +0x28: CuOffset       uint64_t  offset to cutab (pointer-sized)
  // +0x30: FiletabOffset  uint64_t  offset to filetab (pointer-sized)
  // +0x38: PctabOffset    uint64_t  offset to pctab (pointer-sized)
  // +0x40: PclnOffset     uint64_t  offset to pclntable/ftab (pointer-sized)
  // Total header size: alignTo(0x48, PtrSize) bytes (48 + padding for 8-byte
  // ptr)

  void __readHeader(BinaryContext &BC, DataExtractor &DE) override {
    uint64_t Offset = getPclntabHeaderOffset();
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    assert(DE.isValidOffset(Offset) && "Invalid offset");                      \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    Header.Field = readEndianVal(DE, &Offset, FSize);                          \
  }
    PcHeaderFields
#undef F
  }
  void __readHeaderRaw(const BinaryContext &BC, const uint8_t *Data,
                       bool IsLittleEndian) override {
    uint64_t Offset = getPclntabHeaderOffset();
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    const uint8_t *ReadPtr = Data + Offset;                                    \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    Header.Field = readEndianValRaw(&ReadPtr, IsLittleEndian, FSize);          \
    Offset = ReadPtr - Data;                                                   \
  }
    PcHeaderFields
#undef F
  }

  void __writeHeader(BinaryContext &BC, uint8_t *Pclntab) const override {
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    writeEndianVal(BC, Header.Field, FSize, &Pclntab);                         \
  }
    PcHeaderFields
#undef F
  }

  void setNewHeaderOffsets() override {
    Header.FuncnameOffset = 0;
    Header.CuOffset = 0;
    Header.FiletabOffset = 0;
    Header.PctabOffset = 0;
    Header.PclnOffset = getPcHeaderSize();
  }

  bool checkMagic() const override { return Header.Magic == 0xfffffff1; }

public:
  ~Pclntab_v1_20_7() = default;

  static size_t getPcHeaderSize(unsigned Psize) {
    size_t FuncSize = 0;
#define F(Type, IsPointerSize, Field)                                          \
  if (IsPointerSize)                                                           \
    FuncSize += Psize;                                                         \
  else                                                                         \
    FuncSize += sizeof(Type);
    PcHeaderFields
#undef F

        return alignTo(FuncSize, Psize);
  }

#undef PcHeaderFields

  size_t getPcHeaderSize() const override {
    return getPcHeaderSize(Header.PtrSize);
  }

  void setFunctionsCount(uint64_t Count) override { Header.Nfuncs = Count; }

  uint8_t getQuantum() const override { return Header.MinLC; }

  uint8_t getPsize() const override { return Header.PtrSize; }

  uint64_t getFunctionsCount() const override { return Header.Nfuncs; }

  uint64_t getNameOffset() const override {
    return getPclntabHeaderOffset() + Header.FuncnameOffset;
  }

  uint64_t getFiletabOffset() const override {
    return getPclntabHeaderOffset() + Header.FiletabOffset;
  }

  uint64_t getPctabOffset() const override {
    return getPclntabHeaderOffset() + Header.PctabOffset;
  }

  uint64_t getPclntabOffset() const override {
    return getPclntabHeaderOffset() + Header.PclnOffset;
  }

  uint64_t getFunctabOffset() const override {
    return getPclntabHeaderOffset() + Header.PclnOffset;
  }

  void setTextStart(uint64_t Value) override { Header.textStart = Value; }
  void setFuncnameOffset(uint64_t Value) override {
    Header.FuncnameOffset = Value;
  }
  void setCuOffset(uint64_t Value) override { Header.CuOffset = Value; }
  void setFiletabOffset(uint64_t Value) override {
    Header.FiletabOffset = Value;
  }
  void setPctabOffset(uint64_t Value) override { Header.PctabOffset = Value; }
  void setPclnOffset(uint64_t Value) override { Header.PclnOffset = Value; }
  void setNfiles(uint64_t Value) override { Header.Nfiles = Value; }

  Functab getFuncTab(DataExtractor DE, uint64_t *Offset) const override {
    Functab Res;
    Res.Address = DE.getU32(Offset);
    Res.Offset = DE.getU32(Offset);
    return Res;
  }
  Functab getFuncTabRaw(const uint8_t *Data, bool IsLittleEndian,
                        uint64_t *Offset) const override {
    Functab Res;
    const uint8_t *ReadPtr = Data + *Offset;
    Res.Address = readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(uint32_t));
    Res.Offset = readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(uint32_t));
    *Offset = ReadPtr - Data;
    return Res;
  }
};

struct GoFunc_v1_20_7 : GoFunc {
  // runtime/symtab.go
  enum {
    _PCDATA_UnsafePoint = 0,
    _PCDATA_StackMapIndex = 1,
    _PCDATA_InlTreeIndex = 2,
    _PCDATA_ArgLiveIndex = 3,
    _PCDATA_MAX = 4,
    _FUNCDATA_ArgsPointerMaps = 0,
    _FUNCDATA_LocalsPointerMaps = 1,
    _FUNCDATA_StackObjects = 2,
    _FUNCDATA_InlTree = 3,
    _FUNCDATA_OpenCodedDeferInfo = 4,
    _FUNCDATA_ArgInfo = 5,
    _FUNCDATA_ArgLiveInfo = 6,
    _FUNCDATA_WrapInfo = 7,
    _FUNCDATA_MAX = 8,
    _ArgsSizeUnknown = -0x80000000
  };
  enum {
    // PCDATA_UnsafePoint values.
    _PCDATA_UnsafePointSafe = -1,   // Safe for async preemption
    _PCDATA_UnsafePointUnsafe = -2, // Unsafe for async preemption

    // _PCDATA_Restart1(2) apply on a sequence of instructions, within
    // which if an async preemption happens, we should back off the PC
    // to the start of the sequence when resume.
    // We need two so we can distinguish the start/end of the sequence
    // in case that two sequences are next to each other.
    _PCDATA_Restart1 = -3,
    _PCDATA_Restart2 = -4,

    // Like _PCDATA_RestartAtEntry, but back to function entry if async
    // preempted.
    _PCDATA_RestartAtEntry = -5
  };
  enum {
    funcID_normal,
    funcID_abort,
    funcID_asmcgocall,
    funcID_asyncPreempt,
    funcID_cgocallback,
    funcID_debugCallV2,
    funcID_gcBgMarkWorker,
    funcID_goexit,
    funcID_gogo,
    funcID_gopanic,
    funcID_handleAsyncEvent,
    funcID_mcall,
    funcID_morestack,
    funcID_mstart,
    funcID_panicwrap,
    funcID_rt0_go,
    funcID_runfinq,
    funcID_runtime_main,
    funcID_sigpanic,
    funcID_systemstack,
    funcID_systemstack_switch,
    funcID_wrapper,
  };
#define FuncFields                                                             \
  F(uint32_t, EntryOff)                                                        \
  F(int32_t, Name)                                                             \
  F(int32_t, Args)                                                             \
  F(uint32_t, Deferreturn)                                                     \
  F(uint32_t, Pcsp)                                                            \
  F(uint32_t, pcfile)                                                          \
  F(uint32_t, Pcln)                                                            \
  F(uint32_t, Npcdata)                                                         \
  F(uint32_t, CuOffset)                                                        \
  F(uint32_t, StartLine)                                                       \
  F(uint8_t, FuncID)                                                           \
  F(uint8_t, Flag)                                                             \
  F(uint8_t, Unused2)                                                          \
  F(uint8_t, Nfuncdata)

  struct _Func {
#define F(Type, Field) Type Field;
    FuncFields
#undef F
  } __GoFunc;

  // _Func binary layout (runtime/symtab.go):
  // Each _func entry in pclntable (total 48 bytes):
  // +0x00: EntryOff     int32_t   offset from function start to entry point
  // +0x04: Name         int32_t   offset into funcnametab for function name
  // +0x08: Args         int32_t   size of function arguments
  // +0x0c: Deferreturn  uint32_t  offset to deferred call instruction (0 if
  // none) +0x10: Pcsp         uint32_t  offset to PC/SP table +0x14: pcfile
  // uint32_t  offset to PC/file table +0x18: Pcln         uint32_t  offset to
  // PC/line table +0x1c: Npcdata      uint32_t  number of pcdata entries
  // (normally 4) +0x20: CuOffset     uint32_t  compilation unit offset +0x24:
  // StartLine    uint32_t  start line number in source +0x28: FuncID uint8_t
  // special function ID (0=normal, 1=abort, etc.) +0x29: Flag         uint8_t
  // flags (e.g., wrapper, exported) +0x2a: Unused2      uint8_t   padding
  // +0x2b: Nfuncdata    uint8_t   number of funcdata entries
  // Followed by Pcdata[Npcdata] array: uint32_t offsets to PCDATA tables
  // Followed by Funcdata[Nfuncdata] array: uint32_t pointers to function data

  uint32_t Pcdata[_PCDATA_MAX] = {
      0,
  };
  uint32_t Funcdata[_FUNCDATA_MAX] = {
      ~0u,
  };

  int32_t PcdataMax[_PCDATA_MAX] = {
      0,
  };

  void __read(const BinaryContext &BC, const uint8_t *Data, bool IsLittleEndian,
              BinarySection *Section, uint64_t *FuncOffset) override {
#define F(Type, Field)                                                         \
  {                                                                            \
    const uint8_t *ReadPtr = Data + *FuncOffset;                               \
    __GoFunc.Field = readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(Type)); \
    *FuncOffset = ReadPtr - Data;                                              \
  }
    FuncFields
#undef F
  }

  void __write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *SectionData,
               BinarySection *OutputSection, MCSymbol *BeginSym,
               BinaryFunction *FirstBF = nullptr) const override {
    BinaryContext &BC = BF->getBinaryContext();
    assert(__GoFunc.Nfuncdata <= _FUNCDATA_MAX);
    assert(__GoFunc.Npcdata <= _PCDATA_MAX);

#define F(Type, Field)                                                         \
  {                                                                            \
    if constexpr (std::string_view(#Field) == "EntryOff") {                    \
      writeEndianVal(BC, BF->getOutputAddress() - FirstBF->getOutputAddress(), \
                     4, FuncPart);                                             \
    } else {                                                                   \
      writeEndianVal(BC, __GoFunc.Field, sizeof(Type), FuncPart);              \
    }                                                                          \
  }
    FuncFields
#undef F
  }

  virtual void writeFuncdata(BinaryContext &BC, uint8_t **FuncPart,
                             uint8_t *SectionData,
                             BinarySection *OutputSection) override {

    for (uint32_t I = 0; I < getNfuncdata(); ++I) {
      writeEndianVal(BC, Funcdata[I], 4, FuncPart);
    }
  }

  size_t getSize(BinaryContext &BC) const override {
    size_t FuncSize = 0;
#define F(Type, Field) FuncSize += sizeof(Type);
    FuncFields
#undef F
        return FuncSize;
  }

#undef FuncFields

  void disableMetadata() override {
    __GoFunc.pcfile = 0;
    __GoFunc.Pcln = 0;
    __GoFunc.CuOffset = 0;
  }

  int32_t getNameOffset() const override { return __GoFunc.Name; }

  void setNameOffset(int32_t Offset) override { __GoFunc.Name = Offset; }

  uint32_t getDeferreturnOffset() const override {
    return __GoFunc.Deferreturn;
  }

  void setDeferreturnOffset(uint32_t Offset) override {
    __GoFunc.Deferreturn = Offset;
  }

  uint32_t getPcspOffset() const override { return __GoFunc.Pcsp; }

  void setPcspOffset(uint32_t Offset) override {
    assert(Offset && "Zero pcsp!");
    __GoFunc.Pcsp = Offset;
  }

  uint32_t getNpcdata() const override { return __GoFunc.Npcdata; }

  void fixNpcdata() override {
    __GoFunc.Npcdata = _PCDATA_MAX;
    for (int i = _PCDATA_MAX - 1; i >= 0; --i) {
      if (Pcdata[i]) {
        __GoFunc.Npcdata = i + 1;
        return;
      }
    }
  }

  uint32_t getFuncID() const override { return __GoFunc.FuncID; }

  uint32_t getFuncIDForWrapper() const override { return funcID_wrapper; }

  bool hasReservedID(std::string Name) const override {
    return __GoFunc.FuncID != funcID_normal &&
           __GoFunc.FuncID != funcID_wrapper;
  }

  uint8_t getNfuncdata() const override { return __GoFunc.Nfuncdata; }

  unsigned getPcdataUnsafePointIndex() const override {
    return _PCDATA_UnsafePoint;
  }

  unsigned getPcdataStackMapIndex() const override {
    return _PCDATA_StackMapIndex;
  }

  unsigned getPcdataInlTreeIndex() const override {
    return _PCDATA_InlTreeIndex;
  }

  std::optional<unsigned> getPcdataArgLiveIndex() const override {
    return _PCDATA_ArgLiveIndex;
  }

  unsigned getPcdataMaxIndex() const override { return _PCDATA_MAX; }

  size_t getPcdataSize() const override { return sizeof(Pcdata); }

  uint32_t getPcdata(unsigned Index) const override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    return Pcdata[Index];
  }

  void setPcdata(unsigned Index, uint32_t Value) override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    Pcdata[Index] = Value;
  }

  void setPcdataMaxVal(unsigned Index, int32_t Value) override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    PcdataMax[Index] = Value;
  }

  int32_t getPcdataMax(unsigned Index) const override {
    return PcdataMax[Index];
  }

  int getPcdataSafePointVal() const override { return _PCDATA_UnsafePointSafe; }

  int getPcdataUnsafePointVal() const override {
    return _PCDATA_UnsafePointUnsafe;
  }

  int getPcdataRestart1() const override { return _PCDATA_Restart1; }
  int getPcdataRestart2() const override { return _PCDATA_Restart2; }

  unsigned getFuncdataInlTreeIndex() const override {
    return _FUNCDATA_InlTree;
  }

  std::optional<unsigned> getFuncdataWrapInfoIndex() const override {
    return _FUNCDATA_WrapInfo;
  }

  virtual uint64_t readFuncdata(const BinaryContext &BC, DataExtractor DE,
                                uint64_t *Offset) const override {
    return DE.getU32(Offset);
  }
  virtual uint64_t readFuncdataRaw(const BinaryContext &BC, const uint8_t *Data,
                                   bool IsLittleEndian,
                                   uint64_t *Offset) const override {
    const uint8_t *ReadPtr = Data + *Offset;
    uint64_t Val = readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(uint32_t));
    *Offset = ReadPtr - Data;
    return Val;
  }

  uint64_t getFuncdata(unsigned Index) const override {
    assert(Index < _FUNCDATA_MAX && "Invalid index");
    assert(FirstModule && "Module not set");
    if (Funcdata[Index] == ~0u)
      return 0;
    return Funcdata[Index] + FirstModule->getGofunc();
  }

  uint64_t getRawFuncdata(unsigned Index) const override {
    assert(Index < _FUNCDATA_MAX && "Invalid index");
    return Funcdata[Index];
  }

  void setFuncdata(unsigned Index, uint64_t Value) override {
    assert(Index < _FUNCDATA_MAX && "Invalid index");
    Funcdata[Index] = Value;
  }

  ~GoFunc_v1_20_7() = default;
};
struct Module_v1_20_7 : Module {
  ~Module_v1_20_7() = default;

  union ModuleStruct {
    struct {
      uint64_t pcHeader;

      // Function names part
      GoArray funcnametab;

      // Compilation Unit indexes part
      GoArray cutab;

      // Source file names part
      GoArray filetab;

      // Functions pc-relative part
      GoArray pctab;

      // Function - ftab offset table part
      GoArray pclntable;

      // Functions table part
      GoArray ftab;

      uint64_t findfunctab;
      uint64_t minpc, maxpc;
      uint64_t text, etext;
      uint64_t noptrdata, enoptrdata;
      uint64_t data, edata;
      uint64_t bss, ebss;
      uint64_t noptrbss, enoptrbss;
      uint64_t covctrs, ecovctrs;
      uint64_t end, gcdata, gcbss;
      uint64_t types, etypes;
      uint64_t rodata;
      uint64_t gofunc;
      GoArray textsectmap;

      GoArray typelinks;

      GoArray itablinks;

      // Other fields are zeroed/unused in exec
    } m;

    uint64_t a[sizeof(m) / sizeof(uint64_t)];
  } ModuleStruct;

  // ModuleStruct moduledata binary layout (runtime/symtab.go):
  // All fields are pointer-sized (4 or 8 bytes) unless noted:
  // +0x00: pcHeader         uint64_t  address of pclntab pcHeader
  // +0x08: funcnametab      GoArray   {Address, Count[2]} function name strings
  // +0x18: cutab            GoArray   {Address, Count[2]} compilation unit
  // indexes +0x28: filetab          GoArray   {Address, Count[2]} source file
  // names +0x38: pctab            GoArray   {Address, Count[2]} PC-relative
  // table +0x48: pclntable        GoArray   {Address, Count[2]} _func entries
  // +0x58: ftab             GoArray   {Address, Count[2]} functab entries
  // +0x68: findfunctab      uint64_t  address of findfunctab function
  // +0x70: minpc            uint64_t  minimum PC (= runtime.text)
  // +0x78: maxpc            uint64_t  maximum PC (= runtime.etext)
  // +0x80: text             uint64_t  address of runtime.text
  // +0x88: etext            uint64_t  address of runtime.etext
  // +0x90: noptrdata        uint64_t  non-pointer data start
  // +0x98: enoptrdata       uint64_t  non-pointer data end
  // +0xa0: data             uint64_t  data segment start
  // +0xa8: edata            uint64_t  data segment end
  // +0xb0: bss              uint64_t  BSS segment start
  // +0xb8: ebss             uint64_t  BSS segment end
  // +0xc0: noptrbss         uint64_t  non-pointer BSS start
  // +0xc8: enoptrbss        uint64_t  non-pointer BSS end
  // +0xd0: covctrs          uint64_t  coverage counters
  // +0xd8: ecovctrs         uint64_t  coverage counters end
  // +0xe0: end              uint64_t  end of moduledata struct
  // +0xe8: gcdata           uint64_t  GC data pointer
  // +0xf0: gcbss            uint64_t  GC BSS pointer
  // +0xf8: types            uint64_t  types data address
  // +0x100: etypes          uint64_t  types data end
  // +0x108: rodata          uint64_t  read-only data
  // +0x110: gofunc          uint64_t  gofunc data (go1.20+)
  // +0x118: textsectmap     GoArray   text section map
  // +0x128: typelinks       GoArray   type links (go1.18+)
  // +0x138: itablinks       GoArray   itab links
  // Total size: 0x148 bytes (varies by Go version)

  uint64_t getFieldOffset(BinaryContext &BC, uint64_t *Addr) const override {
    unsigned Psize = BC.AsmInfo->getCodePointerSize();
    return (Addr - ModuleStruct.a) * Psize;
  }

  uint64_t *getModule() override { return ModuleStruct.a; }

  size_t getModuleSize() const override { return sizeof(ModuleStruct.m); }

  virtual uint64_t getGofunc() const override { return ModuleStruct.m.gofunc; }

  void setPclntabSize(uint64_t Size) override {
    // Set pctab size
    ModuleStruct.m.pctab.setCount(Size);

    // Set pclntable size
    ModuleStruct.m.pclntable.setCount(Size);
  }

  void setFtabSize(uint64_t Count) override {
    // Fix ftab size; the last entry reserved for maxpc
    ModuleStruct.m.ftab.setCount(Count + 1);
  }

  uint64_t getPcHeaderAddr() const override { return ModuleStruct.m.pcHeader; }

  const GoArray &getFtab() const override { return ModuleStruct.m.ftab; }

  uint64_t getFindfunctab() const override {
    return ModuleStruct.m.findfunctab;
  }

  uint64_t getTypes() const override { return ModuleStruct.m.types; }

  uint64_t getEtypes() const override { return ModuleStruct.m.etypes; }

  uint64_t getText() const override { return ModuleStruct.m.text; }

  const GoArray &getTextsectmap() const override {
    return ModuleStruct.m.textsectmap;
  }

  const GoArray &getTypelinks() const override {
    return ModuleStruct.m.typelinks;
  }

  uint64_t getFuncnameTabAddr() const override {
    return ModuleStruct.m.funcnametab.getAddress();
  }

  StringRef getRuntimeExitName() const override { return "runtime.exit.abi0"; }

  int patch(BinaryContext &BC) override {
    BinaryData *Module = getModuleBD(BC);
    if (!Module) {
      errs() << "BOLT-ERROR: Failed to get firstmoduledata symbol!\n";
      return -1;
    }

    // Update textsectmap array size to 1 if it was non 0 before.
    // Currently we merge all textsecmap entries to single entry.
    // The new entry is located between runtime.text and runtime.etext
    if (ModuleStruct.m.textsectmap.getCount() > 1)
      ModuleStruct.m.textsectmap.setCount(1);

    BinarySection *Section = &Module->getSection();
    std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
    unsigned Psize = BC.AsmInfo->getCodePointerSize();

#define getOffset(Field)                                                       \
  Module->getOffset() + getFieldOffset(BC, &ModuleStruct.m.Field);

#define getValue(Field) (ModuleStruct.m.Field)

    // Fix firsmoduledata pointers
    BinaryData *PclntabSym = BC.getBinaryDataAtAddress(getPcHeaderAddr());
    assert(PclntabSym && "PclntabSym absent");
    BinaryData *FindfunctabSym = BC.getBinaryDataAtAddress(getFindfunctab());
    assert(FindfunctabSym && "FindfunctabSym absent");
    BinaryFunction *FirstBF = getFirstBF(BC, BFs);
    assert(FirstBF && "Text BF absent");
    BinaryFunction *LastBF = getLastBF(BC, BFs);
    assert(LastBF && "Last BF absent");

    // Offsets inside the rebuilt .pclntab are relative to the start of
    // the section: the functab directly follows the pc header, and the
    // pclntable (_func array), whose funcoffs in ftab entries are also
    // section-relative, follows the functab (see
    // GolangMetadataRewriter::pclntabPass). Hence pcHeader, pctab (whose
    // pcdata offsets are section-relative as well) and pclntable all
    // point to the section base. The funcnametab is preserved in the
    // original .gopclntab contents (nameOff values are copied verbatim),
    // so its address is left to the remapping of the original relocation.
#define FirstmoduleFields                                                      \
  F(pcHeader, PclntabSym, 0)                                                   \
  F(pctab.Address, PclntabSym, 0)                                              \
  F(pclntable.Address, PclntabSym, 0)                                          \
  F(ftab.Address, PclntabSym, Pclntab_v1_20_7::getPcHeaderSize(Psize))         \
  F(findfunctab, FindfunctabSym, 0)                                            \
  F(minpc, FirstBF, 0)                                                         \
  F(text, FirstBF, 0)                                                          \
  F(maxpc, LastBF, 0)                                                          \
  F(etext, LastBF, 0)

#define F(Field, Symbol, Addend)                                               \
  {                                                                            \
    uint64_t FieldOffset = getOffset(Field);                                   \
    RemoveRelaReloc(BC, Section, FieldOffset);                                 \
    AddRelaReloc(BC, Symbol->getSymbol(), Section, FieldOffset, Addend);       \
  }
    FirstmoduleFields
#undef F
#undef FirstmoduleFields

        // Fix firstmoduledata static fields
        MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
#define FirstmoduleFields                                                      \
  F(funcnametab.Count[0])                                                      \
  F(funcnametab.Count[1])                                                      \
  F(pctab.Count[0])                                                            \
  F(pctab.Count[1])                                                            \
  F(pclntable.Count[0])                                                        \
  F(pclntable.Count[1])                                                        \
  F(ftab.Count[0])                                                             \
  F(ftab.Count[1])                                                             \
  F(textsectmap.Count[0])                                                      \
  F(textsectmap.Count[1])

#define F(Field)                                                               \
  {                                                                            \
    uint64_t FieldOffset = getOffset(Field);                                   \
    uint64_t FieldVal = getValue(Field);                                       \
    Section->addRelocation(FieldOffset, ZeroSym,                               \
                           static_cast<uint32_t>(Relocation::getAbs(Psize)),   \
                           FieldVal);                                          \
    /* The count fields are not covered by dynamic relocations. In PIE */      \
    /* binaries rewritten with -rewrite, the static relocation patches */      \
    /* are discarded when original section contents are restored, so */        \
    /* queue a pending relocation that is flushed after the restore. */        \
    if (!BC.HasFixedLoadAddress)                                               \
      Section->addPendingRelocation(                                           \
          Relocation{FieldOffset, /*Symbol=*/nullptr,                          \
                     static_cast<uint32_t>(Relocation::getAbs(Psize)),         \
                     FieldVal, /*Value=*/0});                                  \
  }
    FirstmoduleFields
#undef F
#undef FirstmoduleFields

#undef getValue
#undef getOffset
        return 0;
  }
};

struct InlinedCall_v1_20_7 {
  uint8_t FuncID;
  uint8_t Unused[3];
  uint32_t NameOff; // offset into pclntab for name of called function
  uint32_t ParentPc;
  uint32_t StartLine; // line number of start of function (func keyword/TEXT
                      // directive)

  // InlinedCall binary layout (runtime/symtab.go):
  // Total size: 12 bytes
  // +0x00: FuncID     uint8_t   function ID of inlined function (0=normal)
  // +0x01: Unused     uint8_t[3] padding bytes (unused)
  // +0x04: NameOff    uint32_t  offset into pclntab for function name string
  // +0x08: ParentPc   uint32_t  PC offset of inlining call site in parent
  // +0x0c: StartLine  uint32_t  line number of function definition (TEXT
  // directive)
};

} // namespace bolt
} // namespace llvm

#endif
