// Test -rewrite mode on a PIE x86_64 binary with a .data.rel.ro table of
// static pointer initializers, each covered by an R_X86_64_RELATIVE
// relocation: exact function starts (called), a .text interior pointer
// (func+8, equality-checked), and interior pointers into .rodata/.data.
// Verifies that the rewritten addends resolve to the same addresses after
// the dynamic linker applies base+addend at load time.
//
// The -icf=1 variant folds the two identical fold_* functions and checks
// that the folded function's table entry maps to the survivor's address
// (regression for the ICF-folded intra-function offset mapping in
// getNewFunctionAddress()).
//
// REQUIRES: system-linux, native, x86_64-host
// The test executes the produced x86_64 binary, so an x86_64 host is
// required in addition to cross-compilation support.
// RUN: %clang %cflags -nostartfiles -ffreestanding -fPIE -pie -Wl,-q \
// RUN:   -fcf-protection=none -fuse-ld=lld \
// RUN:   -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
// RUN:   -o %t.orig %s -lc
// RUN: %t.orig > %t.orig.out
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt > %t.bolt.out
// RUN: diff %t.orig.out %t.bolt.out
// RUN: FileCheck %s < %t.bolt.out
//
// RUN: llvm-bolt %t.orig -o %t.bolt.icf -rewrite -icf=1
// RUN: %t.bolt.icf > %t.icf.out
// RUN: FileCheck %s --check-prefix=ICF < %t.icf.out
//
// CHECK: pie pointer table ok
// CHECK-NOT: FAIL
// ICF: fold: same
// ICF: pie pointer table ok
// ICF-NOT: FAIL

#include <stddef.h>
#include <unistd.h>

static int add3(int x) {
  volatile int y = x;
  y += 1;
  y += 1;
  y += 1;
  return y;
}

static int mul2(int x) {
  volatile int y = x;
  y *= 2;
  return y;
}

static const int rodata_mid[256] = {1, 2, 3, [32] = 1337, [255] = 42};
static long data_mid[16] = {[8] = 64};

__attribute__((noinline)) static int fold_a(int x) {
  volatile int y = x;
  y ^= 5;
  y += 2;
  return y;
}

__attribute__((noinline)) static int fold_b(int x) {
  volatile int y = x;
  y ^= 5;
  y += 2;
  return y;
}

// Built with file-scope asm so that every entry is a static initializer
// and receives an R_X86_64_RELATIVE relocation in the PIE link.
__asm__(".section .data.rel.ro,\"aw\"\n"
        ".balign 8\n"
        ".globl ptr_tab\n"
        "ptr_tab:\n"
        "  .quad add3\n"
        "  .quad mul2\n"
        "  .quad add3+8\n"
        "  .quad rodata_mid\n"
        "  .quad rodata_mid+128\n"
        "  .quad data_mid+64\n"
        "  .quad fold_a\n"
        "  .quad fold_b\n"
        ".previous\n");

extern void *ptr_tab[8];

static void fail(int n) {
  write(1, "FAIL\n", 5);
  _exit(n);
}

void _start(void) {
  int (**fps)(int) = (int (**)(int))&ptr_tab[0];

  if (fps[0](4) != 7)
    fail(1);
  if (fps[1](21) != 42)
    fail(2);
  if (ptr_tab[1] != (void *)mul2)
    fail(7);
  if (ptr_tab[2] != (void *)((char *)&add3 + 8))
    fail(3);
  if (ptr_tab[3] != (void *)rodata_mid)
    fail(4);
  if (ptr_tab[4] != (void *)&rodata_mid[32])
    fail(5);
  if (ptr_tab[5] != (void *)&data_mid[8])
    fail(6);
  {
    volatile int y = 7;
    y ^= 5;
    y += 2;
    if (fps[6](7) != y)
      fail(8);
    if (fps[7](7) != y)
      fail(10);
  }
  if (ptr_tab[6] != (void *)fold_a || ptr_tab[7] != (void *)fold_b)
    fail(9);
  // Compare through volatile loads: the compiler would otherwise fold the
  // equality to a constant via the link-time addresses of fold_a/fold_b,
  // removing the runtime check this variant exists for.
  {
    void *volatile VTab0 = ptr_tab[6];
    void *volatile VTab1 = ptr_tab[7];
    if (VTab0 == VTab1)
      write(1, "fold: same\n", 11);
    else
      write(1, "fold: distinct\n", 15);
  }

  write(1, "pie pointer table ok\n", 21);
  _exit(0);
}
