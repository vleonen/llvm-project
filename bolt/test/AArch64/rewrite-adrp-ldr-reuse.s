## Regression test: -rewrite must retarget GOT loads that reuse the ADRP
## base register, not only the first paired LDR.
##
## Shape: one ADRP feeds several GOT loads (prologue pair plus epilogue
## reuse loads of the same and a different slot on the same page). Before
## the fix, FixRelaxations paired and retargeted only the first LDR; the
## reuse loads kept the old slot offsets while the ADRP computed the new
## page, loading garbage at runtime (SIGSEGV in the reproducer).
##
## Also verifies the page-phase-preserving .got placement: all slots that
## shared an input page share an output page, so the single retargeted
## ADRP is valid for every reuse load. The clobber negative (base register
## overwritten, fresh ADRP+LDR pair) must keep working as a normal pair.

# REQUIRES: system-linux

# RUN: %clang %cflags -nostartfiles -e _start %s -o %t.orig -Wl,-q -fuse-ld=lld
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
## Runtime: all GOT loads must return the correct values (exit code 0).
# RUN: %t.bolt
## Structural: every GOT load addresses its own slot of the output .got.
# RUN: A=`llvm-readelf -SW %t.bolt | awk '$3==".got" {print $5}'` && P=`printf %x $(( 0x$A & ~0xfff ))` && O1=`printf %x $(( 0x$A & 0xfff ))` && O2=`printf %x $(( (0x$A & 0xfff) + 8 ))` && O3=`printf %x $(( (0x$A & 0xfff) + 16 ))` && llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s -DPAGE=$P -DOFF1=$O1 -DOFF2=$O2 -DOFF3=$O3

## The prologue pair and both reuse loads share the ADRP page; each load
## addresses its own slot (same slot twice, the neighboring slot once).
# CHECK: adrp x8, 0x[[PAGE]]
# CHECK: ldr x9, [x8, #0x[[OFF1]]]
# CHECK: ldr x10, [x8, #0x[[OFF1]]]
# CHECK: ldr x11, [x8, #0x[[OFF2]]]
## Clobber negative: fresh pair with its own ADRP.
# CHECK: adrp x12, 0x[[PAGE]]
# CHECK: ldr x13, [x12, #0x[[OFF3]]]

    .text
    .globl _start
    .p2align 2
_start:
    // Prologue pair (var1).
    adrp x8, :got:var1
    ldr  x9, [x8, :got_lo12:var1]

    // Epilogue reuse loads: no ADRP of their own; the base register x8
    // still holds the GOT page.
    ldr  x10, [x8, :got_lo12:var1]   // same slot as the prologue
    ldr  x11, [x8, :got_lo12:var2]   // different slot, same page

    // Clobber negative: x8 overwritten, fresh ADRP+LDR pair (var3).
    mov  x8, #0
    adrp x12, :got:var3
    ldr  x13, [x12, :got_lo12:var3]

    // Verify all loads returned the correct pointers.
    ldr  w14, [x9]
    ldr  w15, [x10]
    ldr  w16, [x11]
    ldr  w17, [x13]
    cmp  w14, w15
    bne  .Lfail
    cmp  w14, #0x111
    bne  .Lfail
    cmp  w16, #0x222
    bne  .Lfail
    cmp  w17, #0x333
    bne  .Lfail

    mov  x0, #0
    b    .Lexit

.Lfail:
    mov  x0, #1

.Lexit:
    mov  x8, #93
    svc  #0

    .data
    .p2align 3
var1:  .word 0x111
var2:  .word 0x222
var3:  .word 0x333
