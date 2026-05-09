## Check that BOLT correctly handles end-of-section symbols with dynamic
## relocations (R_*_RELATIVE) on AArch64. This tests the relocation
## preservation logic in BinaryEmitter.cpp.

# REQUIRES: target=aarch64{{.*}}, asserts

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --dynamic-linker=/lib/ld-linux-aarch64.so.1
# RUN: llvm-bolt %t.exe -o %t.bolt --print-cfg --debug-only=bolt 2>&1 \
# RUN:   | FileCheck %s

# CHECK: considering symbol _end for function
# CHECK: _end is in the end of .bss
# CHECK-NOT: Binary Function "_end{{.*}}" after building cfg

  .text
  .align 4
  .globl _start
  .type _start,@function
_start:
  ret
  .size _start, .-_start

  .bss
  .align 12
  .zero 4096
  .globl _end
_end:

  .data
  .word 0