# Verifies stack-pointer consistency when instrumenting a *leaf* function (one
# with no calls) on x86-64. Because a leaf function may use the 128-byte red
# zone below %rsp, BOLT's counter snippet must allocate that red zone with
# `leaq -0x80(%rsp),%rsp` before its register pushes and restore it afterwards
# with `leaq 0x80(%rsp),%rsp`, leaving %rsp unchanged (net-zero SP delta).
#
# A canary stored in the red zone must survive every counter snippet. If the
# red-zone bookends ever go missing or get unbalanced, the snippet's pushes
# overwrite the canary and the program returns non-zero.

# REQUIRES: system-linux,bolt-runtime

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags -no-pie %t.o -o %t.exe -Wl,-q

# RUN: llvm-bolt %t.exe --instrument --instrumentation-file=%t.fdata \
# RUN:   -o %t.instrumented
# RUN: %t.instrumented arg1 arg2
# RUN: llvm-objdump --disassemble-symbols=leafFunc %t.instrumented | \
# RUN:   FileCheck %s
# RUN: FileCheck %s --input-file %t.fdata --check-prefix=FDATA

# Both the allocate and the restore bookends must be present, proving the
# counter snippet leaves %rsp with a net-zero delta.
# CHECK: leaq -0x80(%rsp), %rsp
# CHECK: leaq 0x80(%rsp), %rsp

# main called leafFunc once; leafFunc appears as the call target in the profile.
# FDATA: 1 main {{[0-9a-f]+}} 1 leafFunc 0 0 1

  .text
  .globl main
  .type main, %function
  .p2align 4
main:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x2, %edi                # argc >= 3 when invoked with two args
  jb    .Lmain_err
  callq leafFunc                  # main is not a leaf (has a regular call)
  popq  %rbp
  retq
.Lmain_err:
  movl  $1, %eax
  popq  %rbp
  retq
  .size main, .-main

  .globl leafFunc
  .type leafFunc, %function
  .p2align 4
leafFunc:
  # Leaf function: no calls at all. BOLT must treat it as a leaf and protect
  # the red zone around every counter snippet it inserts here.
  movq  $0x12345678, %rax
  movq  %rax, -0x8(%rsp)          # canary in the red zone
  testq %rdi, %rdi                # add control flow so leafFunc has several BBs
  jne   .Lleaf_ok
  movq  $0x12345678, %rax
.Lleaf_ok:
  movq  -0x8(%rsp), %rcx          # read the canary back
  cmpq  %rax, %rcx
  jne   .Lleaf_bad
  xorl  %eax, %eax                # canary intact -> success
  retq
.Lleaf_bad:
  movl  $2, %eax                  # canary corrupted -> failure
  retq
  .size leafFunc, .-leafFunc
