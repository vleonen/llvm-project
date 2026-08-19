## Test the -keep-section-order option of -rewrite mode on AArch64.
## Verify that the relative order of sections in the output matches the
## original binary, and that the default (no option) output is still valid.

// REQUIRES: system-linux

// RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld -o %t.exe %s
// RUN: llvm-bolt %t.exe -o %t.kso -rewrite -keep-section-order
## Assert the order by section address: the section header table index
## order is not guaranteed to be address-sorted.
// RUN: llvm-readelf -SW %t.kso 2>&1 | grep -E "\.(text|eh_frame_hdr|eh_frame|foo\.ro|bar\.ro|data|foo\.rw|bss) " | awk '{print $5, $3}' | sort | awk '{print $2}' | FileCheck %s --check-prefix=CHECK-KSO
## Negative control: without the option the output must still be a valid
## binary with unwind information (order is not asserted).
// RUN: llvm-bolt %t.exe -o %t.def -rewrite
// RUN: llvm-readelf -l %t.def 2>&1 | FileCheck %s --check-prefix=CHECK-DEF

// CHECK-DEF: GNU_EH_FRAME
// CHECK-DEF-NOT: BOLT

## The original section order must be preserved. Note the input places
## read-only sections (including .eh_frame/.eh_frame_hdr) before executable
## sections; the regenerated .eh_frame must inherit the original's position.
// CHECK-KSO: .foo.ro
// CHECK-KSO: .bar.ro
// CHECK-KSO: .eh_frame
// CHECK-KSO: .eh_frame_hdr
// CHECK-KSO: .text
// CHECK-KSO: .data
// CHECK-KSO: .foo.rw
// CHECK-KSO: .bss

.globl _start
.text
_start:
  .cfi_startproc
  .cfi_def_cfa sp, 0
  adr x0, .Lro1
  ldrb w0, [x0]
  mov x8, #93
  svc #0
  .cfi_endproc

.section .foo.ro, "a"
.Lro1:
  .asciz "foo"

.section .bar.ro, "a"
.Lro2:
  .asciz "bar"

.data
.Lval:
  .word 0x42

.section .foo.rw, "aw"
.Lrw:
  .word 0x43

.bss
.Lbss:
  .zero 8
