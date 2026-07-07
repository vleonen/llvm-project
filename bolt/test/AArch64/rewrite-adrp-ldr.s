## Test that -rewrite mode correctly retargets ADRP+LDR GOT loads where
## the GOT entry is at a non-zero page offset. The ADRP addend encodes
## the GOT page address (4KB-aligned) and the LDR addend encodes the
## page offset (bits 0-11). The full GOT entry address is their sum.
## This regression test catches the bug where only the ADRP addend was
## used, producing a page-aligned address that pointed to the wrong slot.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.orig -Wl,-q -static
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite

## Verify the .got section exists in the output.
# RUN: llvm-readelf -SW %t.bolt 2>&1 | FileCheck %s --check-prefix=GOT
# GOT: .got

## After -rewrite, the ADRP+LDR pairs must reference the new GOT page.
## Both var1 and var2 are in the same GOT page; the LDR offsets must
## be distinct (0x0 and 0x8), proving that page offsets are preserved
## from the original GOT layout.
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s

# CHECK: adrp [[REG1:x[0-9]+]], [[GOTPAGE:0x[0-9a-f]+]]
# CHECK-NEXT: ldr {{x[0-9]+}}, [[[REG1]]]
# CHECK: adrp [[REG2:x[0-9]+]], [[GOTPAGE]]
# CHECK-NEXT: ldr {{x[0-9]+}}, [[[REG2]], #0x8]

.text
.globl _start
.p2align 2
_start:
    ## Load var1 GOT entry (offset 0x8 in .got — the first entry after
    ## _DYNAMIC). The nop prevents linker relaxation to adrp+add.
    adrp x0, :got:var1
    nop
    ldr  x0, [x0, :got_lo12:var1]

    ## Load var2 GOT entry (offset 0x10 in .got).
    adrp x1, :got:var2
    nop
    ldr  x1, [x1, :got_lo12:var2]

    ret
.size _start, .-_start

.data
.globl var1
var1:
    .quad 0
.globl var2
var2:
    .quad 0
