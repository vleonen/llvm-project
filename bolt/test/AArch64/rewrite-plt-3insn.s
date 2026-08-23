## Test the -rewrite mode on AArch64 PLT entries without the ADD instruction:
## the 3-instruction form emitted by the Go linker (adrp/ldr/br, loading the
## GOT entry into x16 and branching through it) and the non-lazy .plt.got
## form (adrp/ldr/br/nop). Both must be recognized, re-emitted with GOT
## references retargeted to the relocated .got section, and the rewritten
## binary must run correctly.

// REQUIRES: system-linux

// RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fuse-ld=lld -o %t.orig %s
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt
// RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
// RUN: %t.bolt.kso

## The rewritten binary must still contain the .plt and .got sections.
// RUN: llvm-readelf -SW %t.bolt 2>&1 | FileCheck %s --check-prefix=SEC
// SEC: .plt
// SEC: .got

## The PLT entries must be re-emitted with GOT references retargeted to the
## new .got address, and the calls from _start must land on the relocated
## entries. Entry 1 (Go-style): loads the GOT slot into x16; entry 2
## (non-lazy): loads into x17.
// RUN: llvm-objdump -d --no-show-raw-insn %t.bolt | FileCheck %s

// CHECK: <.plt>:
// CHECK-NEXT: adrp x16, 0x{{[0-9a-f]+}}
// CHECK-NEXT: ldr x16, [x16, #0x{{[0-9a-f]+}}]
// CHECK-NEXT: br x16
// CHECK: adrp x16, 0x{{[0-9a-f]+}}
// CHECK-NEXT: ldr x17, [x16, #0x{{[0-9a-f]+}}]
// CHECK-NEXT: br x17

// CHECK: bl {{.*}}<.plt>
// CHECK: bl {{.*}}<.plt+0xc>

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
