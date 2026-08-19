## Regression test: -rewrite must retarget ADRP+LDR GOT pairs even when
## the ADRP and its paired LDR do not end up in the same basic block.
##
## Two real-world patterns (found on redis-server; unretargeted pairs
## crashed CONFIG GET at runtime):
##  1. The block ends with an unconditional branch to a tail block that
##     contains the paired LDR (compiler-emitted converging paths).
##  2. A branch target after the ADRP splits the block between the ADRP
##     and the paired LDR (fall-through into the LDR).
## FixRelaxations must follow the control flow, find the LDR, and retarget
## both instructions to the GOTENT symbol so the load references the new
## .got address.

# REQUIRES: system-linux

# RUN: %clang %cflags -nostartfiles -e _start %s -o %t.orig -Wl,-q -fuse-ld=lld
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
## The ADRPs must reference the page of the output .got section and the
## LDR offsets must address the first two .got slots in it. With the bug,
## the ADRP kept the old .got page encoded in the input binary and the LDR
## kept the old slot offset, so the load read garbage at runtime.
# RUN: llvm-readelf -SW %t.bolt | awk '$3==".got" {print $5}' > %t.got
# RUN: llvm-objdump -d --no-show-raw-insn %t.bolt > %t.dis
# RUN: python3 %S/Inputs/check-got-loads.py %t.got %t.dis

# CHECK: adrp x0, 0x[[PAGE]]
# CHECK: ldr x0, [x0, #0x[[OFF1]]]
# CHECK: adrp x1, 0x[[PAGE]]
# CHECK: ldr x1, [x1, #0x[[OFF2]]]

.text
.globl _start
.p2align 2

_start:
    ## Pattern 1: ADRP; unconditional branch to tail containing the LDR.
    adrp x0, :got:var1
    b       .Ltail1
    nop
.Ltail1:
    ldr     x0, [x0, :got_lo12:var1]
    str     x0, [sp, #-16]!

    ## Pattern 2: branch target between ADRP and LDR splits the block;
    ## control falls through into the LDR.
    adrp x1, :got:var2
    mov w9, #1
.Lsplit:
    ldr     x1, [x1, :got_lo12:var2]
    str     x1, [sp, #8]

    ldr x0, [sp], #16
    ret

.data
.p2align 3
var1: .quad 0
var2: .quad 0
