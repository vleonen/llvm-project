// Test that -rewrite preserves the original .rodata contents (string literals)
// in a binary that also contains a switch jump table. The jump table is moved
// by BOLT while the string must stay reachable in the output.
//
// REQUIRES: system-linux, native, x86_64-host
// RUN: %clang %cflags -nostartfiles -ffreestanding -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld -O2 -o %t.orig %s
// RUN: %t.orig > %t.orig.out
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt > %t.bolt.out
// RUN: diff %t.orig.out %t.bolt.out

static volatile long g_input = 4;
static const char msg[] = "rewrite-rodata-string\n";

static void write1(long fd, const char *buf, long len) {
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(1), "D"(fd), "S"(buf), "d"(len)
                   : "rcx", "r11", "memory");
}

void _start(void) {
  long result;
  switch (g_input) {
  case 0:
    result = 111;
    break;
  case 1:
    result = 222;
    break;
  case 2:
    result = 333;
    break;
  case 3:
    result = 444;
    break;
  case 4:
    result = 555;
    break;
  case 5:
    result = 666;
    break;
  case 6:
    result = 777;
    break;
  case 7:
    result = 888;
    break;
  default:
    result = -1;
    break;
  }
  write1(1, msg, sizeof(msg) - 1);
  if (result != 555)
    write1(1, "bad\n", 4);
  char digit = (char)('0' + (result % 10));
  write1(1, &digit, 1);
  write1(1, "\n", 1);
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(60), "D"(0)
                   : "rcx", "r11", "memory");
}
