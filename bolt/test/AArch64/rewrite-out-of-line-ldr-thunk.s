## Reproducer for out-of-line load thunks on AArch64.  A toolchain may
## replace a reg-offset load with an unconditional branch to a small thunk
## that performs the load using the base register set up by a preceding
## ADRP and branches back (seen with Go built using -mappingsymbol).
## The R_AARCH64_LDST64_ABS_LO12_NC relocation stays at the branch site
## and describes the thunk's load.  BOLT used to drop relocations on
## branch instructions and re-emit the thunk with its original load
## offset, which goes stale when -rewrite moves data sections.  The fix
## inlines the thunk's load at the branch site so the relocation drives
## re-emission.
##
## Layout trick: dat sits at offset 8 of a 4096-aligned .rodata, so the
## linker-applied LDST64 relocation patches the branch immediate to
## imm12 = 8 >> 3 = 1, i.e. the branch still targets the thunk placed
## exactly 4096 bytes after it (.org 4100).  The input binary is fully
## functional: the thunk loads dat through the ADRP page base.

# REQUIRES: system-linux,target=aarch64{{.*}}

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.orig -nostdlib -static -Wl,-q

## The input binary must run and exit 0 (correct load of dat == 0).
# RUN: %t.orig

# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite

## After -rewrite the branch must be replaced by the inlined load: the
## load must appear directly after the ADRP in _start (the first ADRP in
## .text), with the offset re-encoded against the relocated dat.  In the
## un-fixed output a branch to the thunk stands between them.
# RUN: llvm-objdump -d --no-show-raw-insn -j .text %t.bolt | FileCheck %s

# CHECK: adrp x27
# CHECK-NEXT: ldr x4, [x27{{[^]]*}}]

## The rewritten binary must load the relocated dat and exit 0.
# RUN: %t.bolt

.text
.globl _start
.p2align 2
.type _start, %function
_start:
    adrp x27, dat
    .inst 0x14000400         // b +4096 -> thunk (pre-resolved encoding, so
                              // the only relocation at this site is the
                              // LDST64 below, like in Go binaries linked
                              // with bfd)
1:
    mov w8, #93              // __NR_exit
    and w0, w4, #0xff        // status = low byte of the loaded value
    svc #0
.size _start, . - _start

## Relocation describing the thunk's load, kept at the branch site.
.reloc 4, R_AARCH64_LDST64_ABS_LO12_NC, dat

## The thunk: performs the load and branches back.  Placed exactly 4096
## bytes after the branch (see comment above) so that the linker's
## relocation patch of the branch immediate still targets it.
.org 4100
.globl thunk
.type thunk, %function
thunk:
    ldr x4, [x27, #8]
    b 1b
.size thunk, . - thunk

## dat at page offset 8; the surrounding filler is non-zero so that a
## stale load of page+0 in a broken rewrite produces a non-zero exit.
.section .rodata
.p2align 12
.rept 8
    .byte 0xff
.endr
.globl dat
dat:
    .quad 0
