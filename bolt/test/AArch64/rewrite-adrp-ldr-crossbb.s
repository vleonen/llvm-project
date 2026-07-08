## Regression test: -rewrite must retarget GOT references whose defining
## ADRP sits in a LATER basic block than the loads (loop back-edge shape),
## and must retarget the leftover ADRP itself.
##
## Before the fix, the forward pairing scan never reached the loads (their
## block precedes the ADRP block in layout and is entered via a conditional
## branch); the sweep retargeted the loads to the NEW slot offset while the
## ADRP kept the OLD page, so the rewritten binary silently loaded from
## old-page + new-offset (SIGSEGV at runtime).

# REQUIRES: system-linux,target=aarch64{{.*}}

# RUN: %clang %cflags -nostartfiles -e _start %s -o %t.orig -Wl,-q -fuse-ld=lld
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
## Runtime: the loads must return the correct value (exit code 0).
# RUN: %t.bolt
## Structural: the orphan load and the leftover ADRP must both reference the
## output .got - the ADRP page is the output .got page of var1's slot.
# RUN: A=`llvm-readelf -SW %t.bolt | awk '$3==".got" {print $5}'` && P=`printf %x $(( 0x$A & ~0xfff ))` && O=`printf %x $(( 0x$A & 0xfff ))` && llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s -DPAGE=$P -DOFF=$O

## The loads appear before the ADRP in layout (loop body first).
# CHECK: ldr x23, [x11, #0x[[OFF]]]
# CHECK: ldr x11, [x11, #0x[[OFF]]]
# CHECK: adrp x11, 0x[[PAGE]]

    .text
    .globl _start
    .p2align 2
_start:
    mov  w0, #0
    b    .Linit

.Lbody:
    ldr  x23, [x11, :got_lo12:var1]   // orphan load (base from later block)
    ldr  w24, [x23]
    cmp  w24, #0x111
    b.ne .Lfail
    add  w20, w20, #1
    cmp  w20, #3
    b.lt .Lbody                        // conditional back-edge
    ldr  x11, [x11, :got_lo12:var1]   // last-use orphan, dest==base
    ldr  x24, [x11]
    cmp  w24, #0x111
    b.ne .Lfail
    b    .Lexit

.Linit:                                // ADRP AFTER the body in layout
    adrp x11, :got:var1
    mov  w20, wzr
    cbz  w0, .Lbody                    // conditional entry (taken)

.Lfail:
    mov  x0, #1
    b    .Ldone
.Lexit:
    mov  x0, #0
.Ldone:
    mov  x8, #93
    svc  #0

    .data
    .p2align 3
var1: .word 0x111
