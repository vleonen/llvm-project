## -rewrite must retarget the TLSDESC ADRP+LDR+ADD triple consistently: the
## LDR (resolver load) and the ADD (descriptor address) must use the same
## descriptor page and low-12 offset. The descriptor lives in .got.plt (as bfd
## places it), which -rewrite relocates; the old fallback sweep recomputed the
## ADD's address from .got's page and left it at the stale slot offset
## (add = ldr + 0x10), so the pair disagreed.
##
## Two __thread variables are needed so the descriptor of interest does not
## start on a page boundary (offset 0), which the disassembler prints as
## `[xN]` without an immediate.

# REQUIRES: system-linux, target=aarch64{{.*}}

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.so -Wl,-q -fuse-ld=bfd -shared
# RUN: llvm-bolt %t.so -o %t.bolt.so -rewrite
# RUN: llvm-readelf -rW %t.bolt.so | FileCheck %s --check-prefix=DYN
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt.so | FileCheck %s --check-prefix=CODE

## A relocated TLSDESC descriptor for tls_var2 must be present.
# DYN: R_AARCH64_TLSDESC {{.*}} tls_var2

## The ADRP, LDR and ADD must all encode the same page (ADRP) and low-12
## offset (LDR == ADD).
# CODE: adrp x3, 0x[[PAGE:[0-9a-f]+]]
# CODE-NEXT: ldr x4, [x3, #0x[[OFF:[0-9a-f]+]]]
# CODE-NEXT: add x3, x3, #0x[[OFF]]

    .text
    .globl get_var
    .p2align 2
get_var:
    adrp x3, :tlsdesc:tls_var2
    ldr  x4, [x3, :tlsdesc_lo12:tls_var2]
    add  x3, x3, :tlsdesc_lo12:tls_var2
    blr  x4
    ldr  w0, [x3]
    ret
    .size get_var, .-get_var

    .globl get_var1
    .p2align 2
get_var1:
    adrp x0, :tlsdesc:tls_var1
    ldr  x2, [x0, :tlsdesc_lo12:tls_var1]
    add  x0, x0, :tlsdesc_lo12:tls_var1
    blr  x2
    ldr  w0, [x0]
    ret
    .size get_var1, .-get_var1

    .section .tbss,"awT",@nobits
    .globl tls_var1
    .p2align 2
tls_var1:
    .zero 4
    .globl tls_var2
tls_var2:
    .zero 4
