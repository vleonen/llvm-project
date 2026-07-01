// Verify that -rewrite mode produces a binary that runs correctly.
// This test compiles a C program with multiple functions, global data,
// and string references, rewrites it with -rewrite, and verifies the
// rewritten binary's exit code is 0.
//
// REQUIRES: system-linux
//
// RUN: %clang %cflags -nostdlib -ffreestanding -static -Wl,-q \
// RUN:   -fuse-ld=lld -o %t.orig %s
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt

typedef unsigned long size_t;

static long syscall3(long n, long a, long b, long c) {
  register long x8 asm("x8") = n;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static int my_strlen(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static void write_str(const char *s) {
  syscall3(64, 1, (long)s, my_strlen(s));
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
  syscall3(64, 1, (long)buf, len + 1);
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
  syscall3(93, 0, 0, 0);
}
