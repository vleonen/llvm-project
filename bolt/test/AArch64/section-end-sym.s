## Check that BOLT doesn't consider end-of-section symbols (e.g., _etext) as
## functions for AArch64.

# REQUIRES: target=aarch64{{.*}}, asserts

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown \
# RUN:   %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q
# RUN: llvm-bolt %t.exe -o %t.null --print-cfg --debug-only=bolt 2>&1 \
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
.Lfoo:
  .word 0