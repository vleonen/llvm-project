// Test -rewrite on a PIE AArch64 binary with ICF-folded functions whose
// addresses are stored in a .data.rel.ro pointer table (R_AARCH64_RELATIVE
// relocations on every entry). With -icf=all, fold_a/fold_b merge into one
// survivor; the folded function's table entry must map to the survivor's
// address (regression for the ICF-folded intra-function offset mapping in
// getNewFunctionAddress(), which produced survivor + arbitrary delta).
//
// REQUIRES: system-linux
//
// RUN: %clang %cflags -O2 -Wl,-q %s -o %t.orig
// RUN: %t.orig | FileCheck %s --check-prefix=ORIG
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite -icf=1
// RUN: %t.bolt | FileCheck %s

// ORIG: fold: distinct
// ORIG: calls ok
// CHECK: fold: same
// CHECK: calls ok

#include <stdint.h>
#include <stdio.h>

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

// File-scope asm so every entry is a static initializer carrying an
// R_AARCH64_RELATIVE relocation in the PIE link.
__asm__(".section .data.rel.ro,\"aw\"\n"
        ".balign 8\n"
        ".globl ptr_tab\n"
        "ptr_tab:\n"
        "  .xword fold_a\n"
        "  .xword fold_b\n"
        ".previous\n");

extern uintptr_t ptr_tab[2];

int main(void) {
  volatile int y = 7;
  y ^= 5;
  y += 2;

  if (((int (*)(int))ptr_tab[0])(7) != y)
    return 1;
  if (((int (*)(int))ptr_tab[1])(7) != y)
    return 2;
  if (ptr_tab[0] != (uintptr_t)fold_a || ptr_tab[1] != (uintptr_t)fold_b)
    return 3;

  // Compare through volatile loads: the compiler would otherwise fold
  // tab0 == tab1 to a constant via the link-time addresses of fold_a and
  // fold_b above, removing the runtime check this test exists for.
  volatile uintptr_t *VTab = (volatile uintptr_t *)&ptr_tab[0];
  puts(VTab[0] == VTab[1] ? "fold: same" : "fold: distinct");
  puts("calls ok");
  return 0;
}
