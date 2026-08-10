## Check that -rewrite overrides an explicit -lite with a warning, since
## -rewrite must emit every function and -lite would skip cold/unprofiled
## functions.

// REQUIRES: system-linux
// RUN: %clang %cflags -static -Wl,-q -fcf-protection=none -o %t.exe %s
// RUN: llvm-bolt %t.exe -o %t.bolt -rewrite -lite 2>&1 | FileCheck %s

// CHECK: BOLT-WARNING: -rewrite overrides -lite

.globl _start
.text
_start:
  leaq val(%rip), %rax
  movl (%rax), %eax
  movq $60, %rax
  xorq %rdi, %rdi
  syscall

.data
val:
  .word 0
