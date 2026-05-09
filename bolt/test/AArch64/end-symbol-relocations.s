## Check that BOLT correctly handles end-of-section symbols with relocations
## on AArch64. This tests the getNewEndSymbolValue() functionality.

# REQUIRES: target=aarch64{{.*}}, asserts

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q
# RUN: llvm-bolt %t.exe -o %t.bolt --print-cfg --debug-only=bolt 2>&1 \
# RUN:   | FileCheck %s

# CHECK: considering symbol etext for function
# CHECK: etext is in the end of .text
# CHECK-NOT: Binary Function "etext{{.*}}" after building cfg

  .text
  .align 4
  .globl _start
  .type _start,@function
_start:
  ret
  .size _start, .-_start

  .align 12
  .globl etext
etext:

  .data
  .word 0