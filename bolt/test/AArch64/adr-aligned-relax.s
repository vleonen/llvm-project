# ADRRelaxationPass must convert an ADR whose computed target address is
# page-aligned back to ADRP in place (a value-identical opcode change that
# removes the +/-1MB range restriction) instead of growing the instruction.
# Growing is impossible in non-simple functions (the indirect branch on a
# scratch register below makes the function non-simple) and used to abort
# with:
#   BOLT-ERROR: Cannot relax adr in non-simple function
#
# The ADR-to-a-page-aligned-address pattern is what the GNU linker produces
# when it relaxes an ADRP+LDR GOT pair into ADR+LDR while --emit-relocs
# keeps the original relocations: the ADR computes the page of the GOT slot
# and the LDR supplies the low 12 bits.

# REQUIRES: system-linux,target=aarch64{{.*}}

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux-gnu \
# RUN:   %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.exe -Wl,-q
# RUN: llvm-bolt %t.exe -o %t.bolt --adr-relaxation=true 2>&1 \
# RUN:   | FileCheck %s --check-prefix CHECK-NOERR
# RUN: llvm-objdump --no-print-imm-hex -d --disassemble-symbols=nonsimple %t.bolt \
# RUN:   | FileCheck %s
# RUN: llvm-objdump --no-print-imm-hex -d --disassemble-symbols=main %t.bolt \
# RUN:   | FileCheck %s --check-prefix CHECK-KEEP
# RUN: %t.bolt

  .text
  .align 4
  .global _start
  .type _start, %function
_start:
  bl main
  mov x8, #93             // __NR_exit
  svc #0
  .size _start, .-_start

  .align 4
  .global nonsimple
  .type nonsimple, %function
# Loads the page-aligned data word and returns (value - 0x42) so the exit
# code verifies the converted ADR computed the correct page.
nonsimple:
  stp x29, x30, [sp, #-0x10]!
  adr x0, AlignedGvar
  adr x16, 1f
  br x16
1:
  ldr w0, [x0]
  sub w0, w0, #0x42
  ldp x29, x30, [sp], #0x10
  ret
  .size nonsimple, .-nonsimple

  .align 4
  .global main
  .type main, %function
main:
  stp x29, x30, [sp, #-0x10]!
  bl nonsimple
  // Control case for the regular grow path: an ADR to a page-unaligned
  // address in a simple function is replaced with ADRP+ADD, consuming the
  // following NOP.
  adr x1, Gvar
  nop
  ldp x29, x30, [sp], #0x10
  ret
  .size main, .-main

  .data
  .align 12
  .global AlignedGvar
AlignedGvar:
  .xword 0x42

  .align 3
  .global Gvar
Gvar:
  .xword 0x43

# The page-aligned ADR becomes ADRP with no size change and no error even
# though the function is non-simple.
# CHECK: adrp x0, 0x{{[0-9a-f]+}}
# CHECK-NOT: adr x0

# CHECK-NOERR-NOT: BOLT-ERROR

# The unaligned ADR in the simple function main still uses the grow path:
# ADRP+ADD replacing adr+nop.
# CHECK-KEEP: adrp x1, 0x{{[0-9a-f]+}}
# CHECK-KEEP-NEXT: add x1, x1, #{{[0-9a-f]+}}
