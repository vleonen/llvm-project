## Test the -rewrite mode on a trivial static AArch64 binary.
## Verify that BOLT produces a valid ELF output with correct program headers.

// REQUIRES: system-linux
// RUN: %clang %cflags -static -Wl,-q -o %t.exe %s
// RUN: llvm-bolt %t.exe -o %t.bolt -rewrite
// RUN: llvm-readelf -h %t.bolt | FileCheck %s --check-prefix=CHECK-ELF
// RUN: llvm-readelf -l %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PHDR

// CHECK-ELF: Class:{{.*}}ELF64
// CHECK-ELF: Type:{{.*}}EXEC
// CHECK-ELF: Machine:{{.*}}AArch64

// CHECK-PHDR: PHDR
// CHECK-PHDR: LOAD
// CHECK-PHDR-NOT: BOLT

.globl _start
.text
_start:
  adr x0, .Lval
  ldr w0, [x0]
  mov x8, #93
  svc #0

.data
.Lval:
  .word 0
