## Check that BOLT preserves NOP instructions of different sizes correctly.
## NOPs > 10 bytes are split into multiple instructions.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q
# RUN: llvm-bolt %t.exe -o %t.bolt.exe --keep-nops --relocs --print-finalized \
# RUN:   |& FileCheck --check-prefix=CHECK-BOLT %s
# RUN: llvm-objdump -d %t.bolt.exe | FileCheck %s

  .text
  .globl _start
  .type _start,@function
_start:
  .cfi_startproc
  .nops 1
  .nops 2
  .nops 3
  .nops 4
  .nops 5
  .nops 6
  .nops 7
  .nops 8
  .nops 9
  .nops 10
  .nops 11
  .nops 12
  .nops 13
  .nops 14
  .nops 15

# Check .text section (BOLT output)
# CHECK:      Disassembly of section .text:
# CHECK-EMPTY:
# CHECK-NEXT: 0000000000600000 <_start>:
# CHECK-NEXT:  600000: 90
# CHECK-NEXT:  600001: 66 90
# CHECK-NEXT:  600003: 0f 1f 00
# CHECK-NEXT:  600006: 0f 1f 40 00
# CHECK-NEXT:  60000a: 0f 1f 44 00 00
# CHECK-NEXT:  60000f: 66 0f 1f 44 00 00
# CHECK-NEXT:  600015: 0f 1f 80 00 00 00 00
# CHECK-NEXT:  60001c: 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  600024: 66 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  60002d: 66 2e 0f 1f 84 00 00 00 00 00
# Sizes 11-15: split into 10 + remainder
# CHECK-NEXT:  600037: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  600041: 90
# CHECK-NEXT:  600042: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  60004c: 66 90
# CHECK-NEXT:  60004e: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  600058: 0f 1f 00
# CHECK-NEXT:  60005b: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  600065: 0f 1f 40 00
# CHECK-NEXT:  600069: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-NEXT:  600073: 0f 1f 44 00 00

# CHECK-BOLT:       Size: 1
# CHECK-BOLT-NEXT:  Size: 2
# CHECK-BOLT-NEXT:  Size: 3
# CHECK-BOLT-NEXT:  Size: 4
# CHECK-BOLT-NEXT:  Size: 5
# CHECK-BOLT-NEXT:  Size: 6
# CHECK-BOLT-NEXT:  Size: 7
# CHECK-BOLT-NEXT:  Size: 8
# CHECK-BOLT-NEXT:  Size: 9
# CHECK-BOLT-NEXT:  Size: 10
# Sizes 11-15: split into 10 + remainder
# CHECK-BOLT-NEXT:  Size: 10
# CHECK-BOLT-NEXT:  Size: 1
# CHECK-BOLT-NEXT:  Size: 10
# CHECK-BOLT-NEXT:  Size: 2
# CHECK-BOLT-NEXT:  Size: 10
# CHECK-BOLT-NEXT:  Size: 3
# CHECK-BOLT-NEXT:  Size: 10
# CHECK-BOLT-NEXT:  Size: 4
# CHECK-BOLT-NEXT:  Size: 10
# CHECK-BOLT-NEXT:  Size: 5

# Needed for relocation mode.
  .reloc 0, R_X86_64_NONE

  .size _start, .-_start
  .cfi_endproc
