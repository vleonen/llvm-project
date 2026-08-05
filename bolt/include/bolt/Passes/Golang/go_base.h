//===--------- Passes/Golang/go_base.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_BASE_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_BASE_H

#include "go_common.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>
#include <optional>
namespace llvm {
namespace bolt {

// runtime/symtab.go
struct Functab {
  // before go 1.18 both are pointer-sized,
  // after 1.18 both are int32 and Address is relative to runtime.text
  uint64_t Address;
  uint64_t Offset;

  // Functab binary layout (runtime/symtab.go):
  // Each functab entry maps function address to pclntable offset.
  // Layout varies by Go version:
  //   Pre-1.18: {uint64_t Address, uint64_t Offset} (16 bytes total)
  //   1.18+:    {int32_t Address (relative to text), int32_t Offset} (8 bytes)
  // Address field:
  //   - Pre-1.18: absolute function address
  //   - 1.18+: offset from runtime.text (negative allowed)
  // Offset field: offset into pclntable where _func entry begins
};

// runtime/symtab.go
struct InlinedCall {
  int16_t Parent;
  uint8_t FuncID;
  uint8_t Unused;
  uint32_t File;
  uint32_t Line;
  uint32_t Func;
  uint32_t ParentPc;
};

class GoArray {
public:
  uint64_t Address;
  uint64_t Count[2];

  uint64_t getAddress() const { return Address; }

  void setAddress(uint64_t Addr) { Address = Addr; }

  uint64_t getCount() const { return Count[0]; }

  void setCount(uint64_t C) {
    Count[0] = C;
    Count[1] = C;
  }
};

// moduledata struct
// runtime/symtab.go
// NOTE: Every field size is target-machines pointer size
// NOTE: For some reason array[] fields count is repeated in struct
struct Module {
  virtual ~Module() = 0;

  BinaryData *getModuleBD(BinaryContext &BC) {
    BinaryData *Module = BC.getFirstBinaryDataByName("local.moduledata");
    if (!Module)
      Module = BC.getFirstBinaryDataByName("runtime.firstmoduledata");

    return Module;
  }

  int read(const BinaryContext &BC);
  virtual uint64_t getFieldOffset(BinaryContext &BC, uint64_t *Addr) const = 0;
  virtual int patch(BinaryContext &BC) = 0;
  virtual uint64_t *getModule() = 0;
  virtual size_t getModuleSize() const = 0;
  virtual void setPclntabSize(uint64_t Size) = 0;
  virtual void setFtabSize(uint64_t Count) = 0;
  virtual uint64_t getPcHeaderAddr() const = 0;
  virtual const GoArray &getFtab() const = 0;
  virtual uint64_t getFindfunctab() const = 0;
  virtual uint64_t getTypes() const = 0;
  virtual uint64_t getEtypes() const = 0;
  virtual uint64_t getText() const = 0;
  virtual const GoArray &getTextsectmap() const = 0;
  virtual const GoArray &getTypelinks() const = 0;
  virtual uint64_t getGofunc() const { return 0; }
  virtual StringRef getRuntimeExitName() const { return "runtime.exit"; }
  virtual uint64_t getFuncnameTabAddr() const = 0;
};

std::unique_ptr<struct Module> createGoModule();

// Structure definition in Golang Compiler:
// runtime/symtab.go
// type textsect struct
struct TextsecmapStruct {
  uint64_t Vaddr;    // prelinked section vaddr
  uint64_t End;      // vaddr + section length
  uint64_t Baseaddr; // relocated section address
};

// runtime/symtab.go
class Pclntab {
  uint64_t PclntabHeaderOffset = 0;

  virtual void __readHeader(BinaryContext &BC, DataExtractor &DE) = 0;
  virtual void __readHeaderRaw(const BinaryContext &BC, const uint8_t *Data,
                               bool IsLittleEndian) = 0;
  virtual void __writeHeader(BinaryContext &BC, uint8_t *Pclntab) const = 0;
  virtual bool checkMagic() const = 0;
  virtual void setNewHeaderOffsets() = 0;

protected:
  void setPclntabHeaderOffset(uint64_t Off) { PclntabHeaderOffset = Off; }

  uint64_t getPclntabHeaderOffset() const { return PclntabHeaderOffset; }

public:
  virtual ~Pclntab() = 0;
  int readHeader(BinaryContext &BC, const uint64_t PclntabHeaderAddr);
  int writeHeader(BinaryContext &BC, uint8_t *Pclntab);
  virtual size_t getPcHeaderSize() const = 0;
  virtual void setFunctionsCount(uint64_t Count) = 0;
  virtual uint8_t getQuantum() const = 0;
  virtual uint8_t getPsize() const = 0;
  virtual uint64_t getFunctionsCount() const = 0;
  virtual uint64_t getNameOffset() const = 0;
  virtual uint64_t getFiletabOffset() const = 0;
  virtual uint64_t getPctabOffset() const = 0;
  virtual uint64_t getPclntabOffset() const = 0;
  virtual uint64_t getFunctabOffset() const = 0;
  virtual void setTextStart(uint64_t Value) = 0;
  virtual void setFuncnameOffset(uint64_t Value) = 0;
  virtual void setCuOffset(uint64_t Value) = 0;
  virtual void setFiletabOffset(uint64_t Value) = 0;
  virtual void setPctabOffset(uint64_t Value) = 0;
  virtual void setPclnOffset(uint64_t Value) = 0;
  virtual void setNfiles(uint64_t Value) = 0;

  virtual Functab getFuncTab(DataExtractor DE, uint64_t *Offset) const {
    Functab Res;
    Res.Address = DE.getAddress(Offset);
    Res.Offset = DE.getAddress(Offset);
    return Res;
  }
  virtual Functab getFuncTabRaw(const uint8_t *Data, bool IsLittleEndian,
                                uint64_t *Offset) const = 0;
};

std::unique_ptr<class Pclntab> createGoPclntab();

// runtime/runtime2.go
struct GoFunc {
  virtual ~GoFunc() = 0;
  Module *FirstModule = nullptr;
  uint64_t PcdataOffset = 0;
  uint64_t FuncdataOffset = 0;

  virtual void __read(const BinaryContext &BC, const uint8_t *Data,
                      bool IsLittleEndian, BinarySection *Section,
                      uint64_t *FuncOffset) = 0;

  int read(const BinaryContext &BC, DataExtractor &DE, BinarySection *Section,
           uint64_t *FuncOffset) {
    const uint8_t *Data = (const uint8_t *)DE.getData().data();
    return read(BC, Data, DE.isLittleEndian(), Section, FuncOffset);
  }

  virtual void __write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *Section,
                       BinarySection *OutputSection, MCSymbol *BeginSym,
                       BinaryFunction *FirstBF = nullptr) const = 0;

  virtual size_t getSize(BinaryContext &BC) const = 0;
  virtual uint64_t readFuncdata(const BinaryContext &BC, DataExtractor DE,
                                uint64_t *Offset) const {
    return readEndianVal(DE, Offset, BC.AsmInfo->getCodePointerSize());
  }
  virtual uint64_t readFuncdataRaw(const BinaryContext &BC, const uint8_t *Data,
                                   bool IsLittleEndian,
                                   uint64_t *Offset) const {
    const uint8_t *ReadPtr = Data + *Offset;
    uint64_t Val = readEndianValRaw(&ReadPtr, IsLittleEndian,
                                    BC.AsmInfo->getCodePointerSize());
    *Offset = ReadPtr - Data;
    return Val;
  }

  int read(const BinaryContext &BC, const uint8_t *Data, bool IsLittleEndian,
           BinarySection *Section, uint64_t *FuncOffset) {
    __read(BC, Data, IsLittleEndian, Section, FuncOffset);

    // Read pcdata
    PcdataOffset = *FuncOffset;
    for (uint32_t I = 0; I < getNpcdata(); ++I) {
      const uint8_t *ReadPtr = Data + *FuncOffset;
      setPcdata(I,
                readEndianValRaw(&ReadPtr, IsLittleEndian, sizeof(uint32_t)));
      *FuncOffset = ReadPtr - Data;
    }

    FuncdataOffset = *FuncOffset;
    for (uint32_t I = 0; I < getNfuncdata(); ++I)
      setFuncdata(I, readFuncdataRaw(BC, Data, IsLittleEndian, FuncOffset));

    return 0;
  }
  void setModule(Module *M) { FirstModule = M; }
  int read(const BinaryFunction &BF) {
    if (!BF.isGolang())
      return -1;

    const BinaryContext &BC = BF.getBinaryContext();
    std::unique_ptr<struct Module> FirstModule = createGoModule();
    FirstModule->read(BC);

    const uint64_t PclntabAddr = FirstModule->getPcHeaderAddr();
    if (!PclntabAddr) {
      errs() << "BOLT-ERROR: Pclntab address is zero!\n";
      return -1;
    }

    const BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabAddr);
    if (!PclntabSym) {
      errs() << "BOLT-ERROR: Failed to get pclntab symbol!\n";
      return -1;
    }

    const BinarySection *Section = &PclntabSym->getSection();
    DataExtractor DE =
        DataExtractor(Section->getContents(), BC.AsmInfo->isLittleEndian(),
                      BC.AsmInfo->getCodePointerSize());
    uint64_t FuncOffset = BF.getGolangFunctabOffset();
    return read(BC, DE, nullptr, &FuncOffset);
  }

  virtual void writeFuncdata(BinaryContext &BC, uint8_t **FuncPart,
                             uint8_t *SectionData,
                             BinarySection *OutputSection) {

    *FuncPart = SectionData + alignTo(*FuncPart - SectionData,
                                      BC.AsmInfo->getCodePointerSize());
    for (uint32_t I = 0; I < getNfuncdata(); ++I) {
      uint64_t Address = getFuncdata(I);
      if (Address) {
        auto Section = BC.getSectionForAddress(Address);
        assert(Section && "Invalid Funcdata");
        auto *SecBegin =
            BC.getOrCreateGlobalSymbol(Section->getAddress(), "DATAat");
        uint64_t OffsetInSection = (uint64_t)(*FuncPart - SectionData);
        AddRelaReloc(BC, SecBegin, OutputSection, OffsetInSection,
                     Address - Section->getAddress());
        *FuncPart += BC.AsmInfo->getCodePointerSize();
      } else {
        writeEndianPointer(BC, 0, FuncPart);
      }
    }
  }
  int write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *SectionData,
            BinarySection *OutputSection, MCSymbol *BeginSym,
            BinaryFunction *FirstBF = nullptr) {
    BinaryContext &BC = BF->getBinaryContext();
    __write(BF, FuncPart, SectionData, OutputSection, BeginSym, FirstBF);

    // Write pcdata
    for (uint32_t I = 0; I < getNpcdata(); ++I)
      writeEndianVal(BC, getPcdata(I), sizeof(uint32_t), FuncPart);

    writeFuncdata(BC, FuncPart, SectionData, OutputSection);

    return 0;
  }

  virtual void disableMetadata() = 0;
  virtual int32_t getNameOffset() const = 0;
  virtual void setNameOffset(int32_t Offset) = 0;
  virtual uint32_t getDeferreturnOffset() const = 0;
  virtual void setDeferreturnOffset(uint32_t Offset) = 0;
  virtual uint32_t getPcspOffset() const = 0;
  virtual void setPcspOffset(uint32_t Offset) = 0;
  virtual uint32_t getNpcdata() const = 0;
  virtual void fixNpcdata() = 0;
  virtual uint32_t getFuncID() const = 0;
  virtual uint32_t getFuncIDForWrapper() const = 0;
  virtual bool hasReservedID(std::string Name) const = 0;
  virtual uint8_t getNfuncdata() const = 0;

  // runtime/symtab.go
  virtual unsigned getPcdataUnsafePointIndex() const = 0;
  virtual unsigned getPcdataStackMapIndex() const = 0;
  virtual int getPcdataRestart1() const = 0;
  virtual int getPcdataRestart2() const = 0;
  virtual unsigned getPcdataInlTreeIndex() const = 0;
  // older golang versions don't have such PCDATA
  virtual std::optional<unsigned> getPcdataArgLiveIndex() const {
    return std::nullopt;
  }
  virtual unsigned getPcdataMaxIndex() const = 0;
  virtual size_t getPcdataSize() const = 0;
  virtual uint32_t getPcdata(unsigned Index) const = 0;
  virtual void setPcdata(unsigned Index, uint32_t Value) = 0;
  virtual void setPcdataMaxVal(unsigned Index, int32_t Value) = 0;
  virtual int32_t getPcdataMax(unsigned Index) const = 0;
  virtual int getPcdataSafePointVal() const = 0;
  virtual int getPcdataUnsafePointVal() const = 0;
  virtual unsigned getFuncdataInlTreeIndex() const = 0;
  virtual std::optional<unsigned> getFuncdataWrapInfoIndex() const {
    return std::nullopt;
  }
  virtual uint64_t getFuncdata(unsigned Index) const = 0;
  // obtaining the actual funcdata requires setting FirstModule first to convert
  // offset to vaddr in newer golang versions. But if we want to just see if
  // funcdatas are identical, we can use raw values without creating a module.
  virtual uint64_t getRawFuncdata(unsigned Index) const = 0;
  virtual void setFuncdata(unsigned Index, uint64_t Value) = 0;

  uint32_t getPcdataOffset() const { return PcdataOffset; }

  uint32_t getFuncdataOffset() const { return FuncdataOffset; }

  // Fake index used to store pcsp values
  unsigned getPcspIndex() const { return getPcdataMaxIndex() + 1; }
};

std::unique_ptr<struct GoFunc> createGoFunc();

} // namespace bolt
} // namespace llvm

#endif
