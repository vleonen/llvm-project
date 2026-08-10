// Test -rewrite mode on a dynamic x86_64 binary with PLT entries.
// Verifies that PLT entries are correctly retargeted to the new GOT layout
// and the rewritten binary produces the same output as the original.
//
// Uses -z now (BIND_NOW) since lazy PLT binding (PLT0 resolver + jmp PLT0
// direct branch) is not yet fully supported under -rewrite on x86.
//
// REQUIRES: system-linux, native
// RUN: %clang %cflags -nostartfiles -ffreestanding -Wl,-q,-z,now \
// RUN:   -fcf-protection=none -fuse-ld=lld -no-pie \
// RUN:   -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
// RUN:   -o %t.orig %s -lc
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.orig > %t.orig.out
// RUN: %t.bolt > %t.bolt.out
// RUN: diff %t.orig.out %t.bolt.out
//
// Verify the rewritten binary still has a .plt section.
// RUN: llvm-readelf -S %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PLT
// CHECK-PLT: .plt
//
// Also test with -keep-section-order.
// RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
// RUN: %t.bolt.kso > %t.bolt.kso.out
// RUN: diff %t.orig.out %t.bolt.kso.out

#include <unistd.h>

void _start(void) {
  const char msg[] = "hello-plt\n";
  write(1, msg, 10);
  _exit(0);
}
