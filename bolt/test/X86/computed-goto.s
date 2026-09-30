# REQUIRES: system-linux, x86_64-host

# This test checks that BOLT correctly processes computed-goto dispatch tables:
# label address tables referenced through a table base pre-materialized into a
# register with a RIP-relative LEA, possibly behind a "notrack" (0x3e) prefix,
# with the table entries relocated via dynamic R_X86_64_RELATIVE relocations
# (PIE) or static relocations (non-PIE). Before the fix the dispatching
# indirect jump was not recognized as a jump table (UNKNOWN CONTROL FLOW) and
# the label table entries were not updated after function reordering.

# PIE: label addresses stored in .data.rel.ro as dynamic relocations.
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags -pie %t.o -o %t.pie -Wl,-q
# RUN: llvm-bolt %t.pie -o %t.pie.bolt -reorder-blocks=ext-tsp 2>&1 | FileCheck %s
# CHECK-NOT: BOLT-ERROR
# CHECK-NOT: BOLT-WARNING

# RUN: llvm-bolt %t.pie -o %t.pie.bolt --print-cfg --print-only=main 2>&1 \
# RUN:   | FileCheck --check-prefix=CHECK-CFG %s
# The dispatch jump must be recognized as a jump table.
# CHECK-CFG-NOT: UNKNOWN CONTROL FLOW

# Check that the binary runs and dispatches to the right label (exit code 0).
# RUN: %t.pie
# RUN: %t.pie.bolt
# Check the -rewrite mode output as well.
# RUN: llvm-bolt %t.pie -o %t.pie.rewrite -reorder-blocks=ext-tsp -rewrite 2>&1 | FileCheck %s
# RUN: %t.pie.rewrite

# Check that dynamic relocations against the label table were preserved.
# RUN: llvm-readelf -rW %t.pie.bolt | FileCheck --check-prefix=CHECK-RELOCS %s
# CHECK-RELOCS: R_X86_64_RELATIVE

# Non-PIE: label addresses stored in .data.rel.ro as static relocations,
# "notrack" indirect jump. Use -static as the -no-pie override of the -pie
# flag from %cflags leaves a PT_INTERP header without a dynamic section.
# RUN: %clang %cflags -static %t.o -o %t.nopie -Wl,-q
# RUN: llvm-bolt %t.nopie -o %t.nopie.bolt -reorder-blocks=ext-tsp 2>&1 | FileCheck %s
# RUN: %t.nopie
# RUN: %t.nopie.bolt
# RUN: llvm-bolt %t.nopie -o %t.nopie.rewrite -reorder-blocks=ext-tsp -rewrite 2>&1 | FileCheck %s
# RUN: %t.nopie.rewrite

  .text
  .globl _start
  .p2align 2
  .type _start, @function
_start:
  xorl  %eax, %eax
  leaq  .Ltable(%rip), %rbx
  notrack jmpq *(%rbx,%rax,8)

.Label0:   # Block address taken
  xorl  %edi, %edi
  jmp   .Ldone
.Label1:   # Block address taken
  movl  $1, %edi
  jmp   .Ldone
.Ldone:
  # exit(%edi)
  movl  $60, %eax
  syscall
  hlt
.Lfunc_end:
  .size _start, .Lfunc_end-_start

  .section .data.rel.ro,"aw",@progbits
  .p2align 3
.Ltable:
  .quad .Label0
  .quad .Label1
