# REQUIRES: system-linux
# This test reproduces a bug in the LongJmp pass where the address of a
# secondary entry point is resolved using its *original* (input) address
# instead of the tentative output-layout address (LongJmp.cpp:462,
# getSymbolAddress). Because B's original address sits right at the end of the
# input binary - just below the new .text section where the caller A is placed -
# needsStub() mis-judges A's call to B's secondary entry as being in range and
# inserts no stub. At emission the call targets B's *final* address, which is
# out of the +/-128MB branch range, producing "fixup value out of range".
#
# The pad function between A and B forces A and B more than 256MB apart in
# BOLT's output, while B's original address remains next to the new .text.

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.exe -nostartfiles -fuse-ld=lld -Wl,-q
# RUN: not llvm-bolt %t.exe -o %t.bolt 2>&1 | FileCheck %s

# CHECK: error: fixup value out of range

    .text
    .balign 4
    .globl _start
    .type _start,@function
_start:
    bl    A_mid            // reference A's secondary entry -> A is multi-entry
    ret
    .size _start, .-_start

    .globl A
    .type A,@function
A:
    bl    B_mid            // call to B's secondary entry (triggers the bug)
    bl    A_internal       // internal call -> A is non-simple
A_internal:
    ret
A_mid:                     // secondary entry of A
    ret
    .size A, .-A

    .globl pad
    .type pad,@function
pad:
    ret
    .space 0x10400000      // 260MB: places B >256MB from A in BOLT's output
    .size pad, .-pad

    .globl B
    .type B,@function
B:
    mov   x0, #0
    .globl B_mid
B_mid:                     // secondary entry of B (the mis-resolved target)
    ret
    .size B, .-B

    .reloc 0, R_AARCH64_NONE
