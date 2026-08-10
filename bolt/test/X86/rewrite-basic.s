## Test the -rewrite mode on a trivial static x86_64 binary.
## Verify that BOLT produces a valid ELF output with correct program headers.

// REQUIRES: system-linux
// RUN: %clang %cflags -static -Wl,-q -fcf-protection=none -o %t.exe %s
// RUN: llvm-bolt %t.exe -o %t.bolt -rewrite
// RUN: llvm-readelf -h %t.bolt | FileCheck %s --check-prefix=CHECK-ELF
// RUN: llvm-readelf -l %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PHDR

// CHECK-ELF: Class:{{.*}}ELF64
// CHECK-ELF: Type:{{.*}}EXEC
// CHECK-ELF: Machine:{{.*}}X86-64

// CHECK-PHDR: PHDR
// CHECK-PHDR: LOAD
// CHECK-PHDR-NOT: BOLT

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
