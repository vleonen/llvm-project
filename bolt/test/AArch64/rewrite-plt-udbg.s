## Test the -rewrite mode with -update-debug-sections on a binary with
## symbol-less PLT entries. requiresAddressMap() returns true for all
## functions when -update-debug-sections is used, so updateOutputValues
## iterates PLT functions whose basic blocks were created during
## disassemblePLT. The rewrite emission path defines basic block labels
## for PLT entries, and updateOutputValues skips any remaining undefined
## labels, so the "symbol should be defined" invariant is preserved.

// REQUIRES: system-linux

// RUN: %clang %cflags -g -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fuse-ld=lld -o %t.orig %s
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite -update-debug-sections
// RUN: %t.bolt
// RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -update-debug-sections \
// RUN:   -keep-section-order
// RUN: %t.bolt.kso

.globl _start
.text
_start:
  bl .LpltA
  mov w20, w0
  bl .LpltB
  add w0, w0, w20
  cmp w0, #49
  b.ne 1f
  mov x8, #93
  mov x0, #0
  svc #0
1:
  mov x8, #93
  mov x0, #1
  svc #0
.size _start, .-_start

.globl funcA
funcA:
  mov w0, #42
  ret
.size funcA, .-funcA

.globl funcB
funcB:
  mov w0, #7
  ret
.size funcB, .-funcB

## Go-style 3-instruction PLT entry (packed, 12 bytes). Private labels so
## the entries carry no symbol table entries, like Go linker output where
## calls reference .plt+n offsets.
.section .plt, "ax"
.LpltA:
  adrp x16, .LgotA
  ldr x16, [x16, :lo12:.LgotA]
  br x16

## Non-lazy .plt.got style entry (16 bytes with NOP padding).
.LpltB:
  adrp x16, .LgotB
  ldr x17, [x16, :lo12:.LgotB]
  br x17
  nop

## GOT slots holding resolved function addresses (no dynamic relocations in
## a static binary; BOLT reads the slot values to resolve the PLT targets).
.section .got, "aw"
.p2align 3
.LgotA:
  .xword funcA
.LgotB:
  .xword funcB
