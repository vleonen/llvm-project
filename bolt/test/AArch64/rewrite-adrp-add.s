## Test that -rewrite mode patches ADRP+LDR pairs (GOT loads) but does
## NOT patch ADRP+ADD pairs (direct address computations). ADRP+ADD
## pairs that reference BSS/data sections were already resolved by
## JITLink to their new addresses. Patching them based on the old GOT
## page range produces false positives when new BSS addresses happen to
## overlap the old GOT range (e.g., with -keep-section-order).

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.orig -Wl,-q -static
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite

## Verify the output has a .got section (so the ADRP patching loop runs).
# RUN: llvm-readelf -SW %t.bolt 2>&1 | FileCheck %s --check-prefix=GOT
# GOT: .got

## After -rewrite: BSS is at 0x230000, GOT is at 0x220000.
## The ADRP+ADD must reference the BSS page (0x230000),
## and the ADRP+LDR must reference the GOT page (0x220000).
## If the ADRP+ADD is incorrectly patched (the bug), it would
## reference the GOT page (0x220000) instead of BSS (0x230000).
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s

## Also verify with -keep-section-order.
# RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
# RUN: llvm-readelf -SW %t.bolt.kso 2>&1 | FileCheck %s --check-prefix=GOT
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt.kso | FileCheck %s

# CHECK: adrp x0, 0x230000
# CHECK-NEXT: add x0, x0, #0x{{[0-9a-f]+}}
# CHECK: adrp x1, 0x220000
# CHECK-NEXT: ldr x1, [x1]

.text
.globl _start
.p2align 2
_start:
    ## ADRP+ADD for direct BSS access.
    ## The nop prevents the linker from relaxing this to adr+nop.
    adrp x0, bss_var
    nop
    add  x0, x0, :lo12:bss_var

    ## ADRP+LDR for GOT access.
    ## The nop prevents the linker from relaxing this to adrp+add
    ## or adr+nop, keeping the GOT indirection intact.
    adrp x1, :got:bss_var
    nop
    ldr  x1, [x1, :got_lo12:bss_var]

    ret
.size _start, .-_start

.bss
.globl bss_var
bss_var:
    .skip 8
