## Test the -rewrite mode with .eh_frame/.eh_frame_hdr preservation on
## AArch64. Verify that the rewritten binary contains a non-empty .eh_frame
## with FDEs for all functions, a valid .eh_frame_hdr/PT_GNU_EH_FRAME that
## references the new .eh_frame address, and no stale relocation sections
## or BOLT-internal sections.

// REQUIRES: system-linux

// RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld -o %t.exe %s
// RUN: llvm-bolt %t.exe -o %t.bolt -rewrite
// RUN: llvm-readelf -S %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-SEC
// RUN: llvm-readelf -l %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PHDR
// RUN: llvm-readelf --unwind %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-UNWIND

// CHECK-SEC: .eh_frame_hdr
// CHECK-SEC: .eh_frame
// CHECK-SEC-NOT: .bolt.
// CHECK-SEC-NOT: .rela.eh_frame

// CHECK-PHDR: GNU_EH_FRAME
// CHECK-PHDR-NOT: BOLT

## Two functions with CFI produce two FDE records.
// CHECK-UNWIND: EHFrameHeader {
// CHECK-UNWIND: eh_frame_ptr: [[EHF:0x[0-9a-f]+]]
// CHECK-UNWIND: fde_count: 2
## The header's eh_frame_ptr must match the .eh_frame section address, and
## FDE records must point back to the CIE inside the same section.
// CHECK-UNWIND: .eh_frame section at offset {{0x[0-9a-f]+}} address [[EHF]]:
// CHECK-UNWIND-COUNT-2: FDE
// CHECK-UNWIND: cie=[[[EHF]]]

.globl _start
.text
_start:
  .cfi_startproc
  .cfi_def_cfa sp, 0
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa sp, 16
  .cfi_offset x29, -16
  .cfi_offset x30, -8
  mov x29, sp
  bl func2
  ldp x29, x30, [sp], #16
  .cfi_def_cfa sp, 0
  .cfi_restore x29
  .cfi_restore x30
  mov x8, #93
  svc #0
  .cfi_endproc

.globl func2
func2:
  .cfi_startproc
  .cfi_def_cfa sp, 0
  mov w0, #42
  ret
  .cfi_endproc

.section .note.my.note, "a"
  .word 0
