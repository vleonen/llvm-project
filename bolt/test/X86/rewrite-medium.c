// Test -rewrite mode on a self-contained static x86_64 binary with multiple
// functions, global data, and PC-relative relocations. Verifies that the
// rewritten binary produces correct runtime output identical to the original.
//
// REQUIRES: system-linux, native, x86_64-host
// The test executes the produced x86_64 binary, so an x86_64 host is
// required in addition to cross-compilation support.
// RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld -o %t.orig %s
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.orig > %t.orig.out
// RUN: %t.bolt > %t.bolt.out
// RUN: diff %t.orig.out %t.bolt.out
//
// Also test with optimization flags.
// RUN: llvm-bolt %t.orig -o %t.bolt.opt -rewrite \
// RUN:   -reorder-blocks=ext-tsp -split-functions
// RUN: %t.bolt.opt > %t.bolt.opt.out
// RUN: diff %t.orig.out %t.bolt.opt.out
//
// Verify clean section names (no .bolt.* prefixes).
// RUN: llvm-readelf -S %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-SEC
// CHECK-SEC: .text
// CHECK-SEC: .data
// CHECK-SEC-NOT: .bolt.org
// CHECK-SEC-NOT: .bolt.new
//
// Verify correct LOAD segment count.
// RUN: llvm-readelf -l %t.bolt 2>&1 | FileCheck %s --check-prefix=CHECK-PHDR
// CHECK-PHDR: PHDR
// CHECK-PHDR: LOAD
// CHECK-PHDR: LOAD
// CHECK-PHDR: LOAD

typedef unsigned long size_t;

static long syscall3(long n, long a, long b, long c) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c)
                   : "rcx", "r11", "memory");
  return ret;
}

static int my_strlen(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static void write_str(const char *s) {
  syscall3(1, 1, (long)s, my_strlen(s));
}

static char *uitoa(unsigned int val, char *buf) {
  char *p = buf;
  if (val == 0) {
    *p++ = '0';
    return p;
  }
  while (val > 0) {
    *p++ = '0' + (val % 10);
    val /= 10;
  }
  return p;
}

static void write_uint(unsigned int val) {
  char buf[16];
  char *end = uitoa(val, buf);
  int len = end - buf;
  for (int i = 0; i < len / 2; i++) {
    char tmp = buf[i];
    buf[i] = buf[len - 1 - i];
    buf[len - 1 - i] = tmp;
  }
  buf[len] = '\n';
  syscall3(1, 1, (long)buf, len + 1);
}

static const char *messages[] = {"fib(", ") = ", "sum=", "ok\n"};
static int results[8];

static int fib(int n) {
  if (n <= 1)
    return n;
  return fib(n - 1) + fib(n - 2);
}

void _start(void) {
  unsigned int sum = 0;
  for (int i = 0; i < 8; i++) {
    results[i] = fib(i);
    sum += results[i];
  }
  for (int i = 0; i < 8; i++) {
    write_str(messages[0]);
    write_uint(i);
    write_str(messages[1]);
    write_uint(results[i]);
  }
  write_str(messages[2]);
  write_uint(sum);
  write_str(messages[3]);
  syscall3(60, 0, 0, 0);
}
