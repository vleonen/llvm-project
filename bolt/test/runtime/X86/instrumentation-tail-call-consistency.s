# Verifies stack-pointer consistency when instrumenting a function whose only
# call is a (direct) tail call. BOLT treats such a function as a leaf, so its
# counter snippet must protect the red zone with balanced
# `leaq -0x80(%rsp),%rsp` / `leaq 0x80(%rsp),%rsp` bookends.
#
# This complements instrumentation-tail-call.s (which only checks the restore
# half and a conditional tail call): here both bookends are asserted and a
# red-zone canary must survive, so any regression that drops or unbalances the
# allocation is caught.

# REQUIRES: system-linux,bolt-runtime

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags -no-pie %t.o -o %t.exe -Wl,-q

# RUN: llvm-bolt %t.exe --instrument --instrumentation-file=%t.fdata \
# RUN:   -o %t.instrumented
# RUN: %t.instrumented arg1 arg2
# RUN: llvm-objdump --disassemble-symbols=tcCaller %t.instrumented | \
# RUN:   FileCheck %s
# RUN: FileCheck %s --input-file %t.fdata --check-prefix=FDATA

# Both the allocate and the restore bookends must be present (net-zero SP delta).
# CHECK: leaq -0x80(%rsp), %rsp
# CHECK: leaq 0x80(%rsp), %rsp

# The direct tail call tcCaller -> targetFunc must be recorded with count 1.
# FDATA: 1 tcCaller {{[0-9a-f]+}} 1 targetFunc 0 0 1

  .text
  .globl main
  .type main, %function
  .p2align 4
main:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x2, %edi
  jb    .Lmain_err
  callq tcCaller
  popq  %rbp
  retq
.Lmain_err:
  movl  $1, %eax
  popq  %rbp
  retq
  .size main, .-main

  .globl tcCaller
  .type tcCaller, %function
  .p2align 4
tcCaller:
  # Leaf: only a tail call. The red zone is therefore usable; a canary stored
  # there must survive the counter snippets that run before the tail call.
  movq  $0x12345678, %rax
  movq  %rax, -0x8(%rsp)          # canary in the red zone
  cmpl  $0x2, %edi                # control flow so this isn't a trivial 1-BB func
  jb    .Ltc_err
  movq  -0x8(%rsp), %rax          # canary intact?
  cmpq  $0x12345678, %rax
  jne   .Ltc_err
  jmp   targetFunc                # direct tail call (only call -> leaf)
.Ltc_err:
  movl  $4, %eax
  retq
  .size tcCaller, .-tcCaller

  .globl targetFunc
  .type targetFunc, %function
  .p2align 4
targetFunc:
  xorl  %eax, %eax
  retq
  .size targetFunc, .-targetFunc
