## Verify that -rewrite mode patches .got entries that reference data.
## In a statically linked binary the linker resolves GOT slots for globals
## at link time (R_AARCH64_ABS64 in .rela.got, no dynamic relocations), so
## slot values are input data addresses. When -rewrite moves .data, these
## slots must be patched to the new addresses; otherwise the output keeps
## stale input addresses and GOT-indirect loads read garbage.
##
## The nops between ADRP and LDR prevent the linker from relaxing the
## GOT-indirect loads into direct ADRP+ADD addressing.

# REQUIRES: system-linux

# RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
# RUN:   -fuse-ld=lld -o %t.orig %s
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
# RUN: %t.bolt
# RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
# RUN: %t.bolt.kso

## The first .got slot must hold the output address of var1 (the first
## variable of .data). Compute the byte-swapped (memory order) hex words of
## the output .data address and check they appear in the .got hex dump.
## With the bug, the slot keeps the input address and this check fails.
# RUN: D=`llvm-readelf -SW %t.bolt | awk '$3==".data" {print $5}'` && \
# RUN:   V=$(( 0x$D )) && \
# RUN:   W=`printf "%02x%02x%02x%02x 00000000" \
# RUN:     $(( V & 0xff )) $(( (V>>8) & 0xff )) \
# RUN:     $(( (V>>16) & 0xff )) $(( (V>>24) & 0xff ))` && \
# RUN:   llvm-readelf -x .got %t.bolt | grep -q "$W"

.globl _start
.text
_start:
  mov w21, #0                 // failure count

  // var1
  adrp x0, :got:var1
  nop
  ldr  x0, [x0, :got_lo12:var1]
  ldr  w1, [x0]
  cmp  w1, #0x111
  cinc w21, w21, ne

  // var2
  adrp x0, :got:var2
  nop
  ldr  x0, [x0, :got_lo12:var2]
  ldr  w1, [x0]
  cmp  w1, #0x222
  cinc w21, w21, ne

  // var3
  adrp x0, :got:var3
  nop
  ldr  x0, [x0, :got_lo12:var3]
  ldr  w1, [x0]
  cmp  w1, #0x333
  cinc w21, w21, ne

  // var4 (64-bit)
  adrp x0, :got:var4
  nop
  ldr  x0, [x0, :got_lo12:var4]
  ldr  x1, [x0]
  mov  x2, #0x444
  cmp  x1, x2
  cinc w21, w21, ne

  // exit(failures)
  mov x8, #93
  mov x0, x21
  svc #0
.size _start, .-_start

.data
.globl var1
var1:
  .word 0x111
.globl var2
var2:
  .word 0x222
.globl var3
var3:
  .word 0x333
.globl var4
.p2align 3
var4:
  .quad 0x444
