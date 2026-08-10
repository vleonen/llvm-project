// Test that -rewrite rejects binaries containing functions that cannot all
// be relocated. -rewrite replaces the main code section with BOLT-emitted
// functions, so excluding functions from processing via -funcs, -skip-funcs
// or -max-funcs would leave their code behind while their old addresses are
// reused. BOLT must error out instead of silently producing a broken binary.
// A plain -rewrite run on the same binary must still succeed.
//
// REQUIRES: system-linux, native, x86_64-host
//
// RUN: %clang -nostartfiles -ffreestanding -Wl,-q -fcf-protection=none \
// RUN:   -fuse-ld=lld -o %t.exe %s
//
// Positive control: plain -rewrite succeeds.
// RUN: llvm-bolt %t.exe -o %t.bolt -rewrite
//
// RUN: not llvm-bolt %t.exe -o %t.out -rewrite -funcs=func1 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CHECK-FUNCS
// RUN: not llvm-bolt %t.exe -o %t.out -rewrite -skip-funcs=func2 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CHECK-SKIP
// RUN: not llvm-bolt %t.exe -o %t.out -rewrite -max-funcs=1 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CHECK-MAXFUNCS
//
// CHECK-FUNCS: BOLT-ERROR: -rewrite requires all functions in .text to be relocated, but {{[0-9]+}} function(s) cannot be emitted:
// CHECK-FUNCS-NOT: func1
// CHECK-FUNCS: func2
// CHECK-FUNCS: func3
// CHECK-SKIP: BOLT-ERROR: -rewrite requires all functions in .text to be relocated, but {{[0-9]+}} function(s) cannot be emitted:
// CHECK-SKIP: func2
// CHECK-MAXFUNCS: BOLT-ERROR: -rewrite requires all functions in .text to be relocated, but {{[0-9]+}} function(s) cannot be emitted:

  .text
  .globl _start
  .type _start, @function
_start:
  call func1
  mov $60, %rax
  xor %rdi, %rdi
  syscall

  .globl func1
  .type func1, @function
func1:
  ret

  .globl func2
  .type func2, @function
func2:
  ret

  .globl func3
  .type func3, @function
func3:
  ret
