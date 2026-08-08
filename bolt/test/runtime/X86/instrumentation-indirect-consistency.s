# Verifies stack-pointer consistency when instrumenting an *indirect* call on
# x86-64. The instrumented indirect-call snippet saves registers with a run of
# pushes and must balance them with matching pops before re-executing the
# original indirect call, so that %rsp (and the preserved %rdi / flags) are
# restored to their pre-snippet values.
#
# Runtime correctness depends on this: %rdi must reach the callee unchanged
# (the callee returns 0 only for the expected argument), and the caller's frame
# canary must be intact after the call returns (proving the return address and
# %rsp were consistent).

# REQUIRES: system-linux,bolt-runtime

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-unknown %s -o %t.o
# RUN: %clang %cflags -no-pie %t.o -o %t.exe -Wl,-q

# RUN: llvm-bolt %t.exe --instrument --instrumentation-file=%t.fdata \
# RUN:   -o %t.instrumented
# RUN: %t.instrumented arg1 arg2
# RUN: llvm-objdump --disassemble-symbols=indCaller %t.instrumented | \
# RUN:   FileCheck %s
# RUN: FileCheck %s --input-file %t.fdata --check-prefix=FDATA

# The indirect-call snippet: after the handler returns, four pops balance the
# four pushes, then the original indirect call is re-executed with %rsp and
# the argument registers restored.
# CHECK: callq {{.*}}__bolt_instr_ind_call_handler_func
# CHECK: popq %r11
# CHECK: popq %r11
# CHECK: popq %rdi
# CHECK: popfq
# CHECK: callq *%r11

# The indirect call indCaller -> callee must be recorded with count 1.
# FDATA: 1 indCaller {{[0-9a-f]+}} 1 callee 0 0 1

  .data
  .p2align 3
func_ptr:
  .quad callee
  .size func_ptr, .-func_ptr

  .text
  .globl main
  .type main, %function
  .p2align 4
main:
  pushq %rbp
  movq  %rsp, %rbp
  cmpl  $0x2, %edi
  jb    .Lmain_err
  callq indCaller
  popq  %rbp
  retq
.Lmain_err:
  movl  $1, %eax
  popq  %rbp
  retq
  .size main, .-main

  .globl indCaller
  .type indCaller, %function
  .p2align 4
indCaller:
  # Not a leaf (performs a regular indirect call): it sets up a real frame and
  # addresses its canary via %rbp, so it does not rely on the red zone.
  pushq %rbp
  movq  %rsp, %rbp
  subq  $0x10, %rsp
  movq  $0x12345678, %rax
  movq  %rax, -0x8(%rbp)          # canary within the frame
  movl  $55, %edi                 # argument the callee expects to receive
  callq *func_ptr(%rip)           # indirect call via a global function pointer
  movq  -0x8(%rbp), %rcx          # frame canary intact after the call returned?
  cmpq  $0x12345678, %rcx
  jne   .Lind_bad
  testl %eax, %eax                # callee confirmed %rdi was preserved?
  jne   .Lind_bad
  xorl  %eax, %eax
  addq  $0x10, %rsp
  popq  %rbp
  retq
.Lind_bad:
  movl  $5, %eax
  addq  $0x10, %rsp
  popq  %rbp
  retq
  .size indCaller, .-indCaller

  .globl callee
  .type callee, %function
  .p2align 4
callee:
  # Returns 0 iff the argument register holds the expected value, proving the
  # instrumentation snippet preserved %rdi across the handler call.
  cmpl  $55, %edi
  jne   .Lcallee_bad
  xorl  %eax, %eax
  retq
.Lcallee_bad:
  movl  $1, %eax
  retq
  .size callee, .-callee
