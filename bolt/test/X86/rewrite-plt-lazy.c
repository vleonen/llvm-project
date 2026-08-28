// Test -rewrite mode on a dynamic x86_64 binary with lazily bound PLT
// entries. Verifies that the PLT0 resolver header, the lazy stubs and their
// .got.plt entries are correctly re-emitted and retargeted, so that the
// first call through every PLT entry resolves via _dl_runtime_resolve and
// the rewritten binary produces the same output as the original.
//
// Unlike rewrite-plt.c this test keeps the default lazy binding.
//
// REQUIRES: system-linux, native, x86_64-host
// The test executes the produced x86_64 binary, so an x86_64 host is
// required in addition to cross-compilation support.
//
// Plain lazy PLT: stubs with "push $Idx; jmp PLT0" tails.
// RUN: %clang %cflags -nostartfiles -ffreestanding -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld -no-pie \
// RUN:   -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
// RUN:   -o %t.orig %s -lc
// RUN: %t.orig > %t.orig.out
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt > %t.bolt.out
// RUN: diff %t.orig.out %t.bolt.out
//
// CET lazy PLT: endbr64 + .plt.sec stubs next to the .plt lazy stubs.
// RUN: %clang %cflags -nostartfiles -ffreestanding -Wl,-q \
// RUN:   -fcf-protection=full -fuse-ld=lld -no-pie \
// RUN:   -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
// RUN:   -o %t.cet.orig %s -lc
// RUN: %t.cet.orig > %t.cet.orig.out
// RUN: llvm-bolt %t.cet.orig -o %t.cet.bolt -rewrite
// RUN: %t.cet.bolt > %t.cet.bolt.out
// RUN: diff %t.cet.orig.out %t.cet.bolt.out
//
// Verify the rewritten binaries still have .plt sections.
// RUN: llvm-readelf -S %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PLT
// RUN: llvm-readelf -S %t.cet.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PLT
// CHECK-PLT: .plt
//
// Also test with -keep-section-order.
// RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
// RUN: %t.bolt.kso > %t.bolt.kso.out
// RUN: diff %t.orig.out %t.bolt.kso.out

#include <stddef.h>
#include <string.h>
#include <unistd.h>

void _start(void) {
  const char msg[] = "hello-lazy-plt\n";
  const size_t len = strlen(msg);
  write(1, msg, len);
  write(1, msg, len);
  _exit(0);
}
