# Verifies stack-pointer consistency for the most stack-sensitive combination
# on x86-64: a *leaf* function whose only call is an *indirect tail call*.
#
# BOLT classifies this function as a leaf (its only call is a tail call), so
# every counter snippet must protect the red zone with balanced
# `leaq -0x80(%rsp),%rsp` / `leaq 0x80(%rsp),%rsp` bookends. Additionally, the
# indirect-tail-call snippet must balance its four pushes with four pops before
# re-executing the original tail call as `jmpq *%r11` (not `callq`).
#
# A red-zone canary must survive the counter snippets, and the tail-callee must
# return through the caller's stack frame, which only works if %rsp is restored
# to its pre-snippet value before the indirect jump.
#
# Notes on construction:
#  - The indirect target is made %rsp-relative (a .data pointer would be treated
#    as a jump table, and a bare `jmpq *%reg` is not recognized as instrumentable).
#  - The tail-call block ends with `leave` (a function epilogue). BOLT only
#    recognises an indirect jump as a tail call when its block contains an
#    epilogue (`leave`/`pop`); without it the function is rejected.

# REQUIRES: system-linux,bolt-runtime

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags -no-pie %t.o -o %t.exe -Wl,-q

# RUN: llvm-bolt %t.exe --instrument --instrumentation-file=%t.fdata \
# RUN:   -o %t.instrumented
# RUN: %t.instrumented arg1 arg2
# RUN: llvm-objdump --disassemble-symbols=itcCaller %t.instrumented | \
# RUN:   FileCheck %s
# RUN: FileCheck %s --input-file %t.fdata --check-prefix=FDATA

# Leaf -> red-zone bookends around every counter snippet.
# CHECK: leaq -0x80(%rsp), %rsp
# CHECK: leaq 0x80(%rsp), %rsp

# Indirect tail call: handler, then four balancing pops, then the re-executed
# jump (jmpq, not callq).
# CHECK: callq {{.*}}__bolt_instr_ind_tailcall_handler_func
# CHECK: popq %r11
# CHECK: popq %r11
# CHECK: popq %rdi
# CHECK: popfq
# CHECK: jmpq *%r11

# The indirect tail call itcCaller -> targetFunc must be recorded with count 1.
# FDATA: 1 itcCaller {{[0-9a-f]+}} 1 targetFunc 0 0 1

  .text
  .globl main
  .type main, %function
  .p2align 4
main:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x2, %edi
  jb    .Lmain_err
  callq itcCaller
  popq  %rbp
  retq
.Lmain_err:
  movl  $1, %eax
  popq  %rbp
  retq
  .size main, .-main

  .globl itcCaller
  .type itcCaller, %function
  .p2align 4
itcCaller:
  # Leaf: its only call is an indirect tail call, so it may use the red zone.
  # A real frame (pushq %rbp / leave) is set up so the tail-jump block contains
  # an epilogue, which is what makes BOLT recognise the indirect jump as a tail
  # call.
  pushq %rbp
  movq  %rsp, %rbp
  movq  $0x12345678, %rax
  movq  %rax, -0x8(%rsp)          # canary in the red zone
  cmpl  $0x2, %edi                # control flow so this isn't a trivial 1-BB func
  jb    .Litc_err
  movq  -0x8(%rsp), %rcx          # canary intact?
  cmpq  $0x12345678, %rcx
  jne   .Litc_err
  leaq  targetFunc(%rip), %rax
  movq  %rax, -0x8(%rsp)          # place the indirect target at a known stack slot
  leave                           # epilogue -> the next jmp is recognised as a tail call
  jmpq  *-0x10(%rsp)              # indirect tail call (rsp-relative, only call -> leaf)
.Litc_err:
  movl  $6, %eax
  leave
  retq
  .size itcCaller, .-itcCaller

  .globl targetFunc
  .type targetFunc, %function
  .p2align 4
targetFunc:
  xorl  %eax, %eax
  retq
  .size targetFunc, .-targetFunc
