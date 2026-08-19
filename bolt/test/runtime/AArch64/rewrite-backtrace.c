// Verify that frame pointer/unwind-based backtrace() works after -rewrite
// on AArch64. Walking the stack out of a deep call chain requires correct
// .eh_frame/.eh_frame_hdr for the rewritten code. Symbol names are not
// asserted: glibc backtrace_symbols() only resolves names exported from
// the dynamic symbol table.
//
// REQUIRES: system-linux
//
// RUN: %clang %cflags -O1 -Wl,-q -fuse-ld=lld -o %t.orig %s
// RUN: %t.orig
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt
// RUN: llvm-bolt %t.orig -o %t.bolt.kso -rewrite -keep-section-order
// RUN: %t.bolt.kso

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void __attribute__((noinline)) sink(void **frames) {
  volatile int x = 0;
  (void)x;
  // Deep chain: leaf, level3, level2, level1, main (+ libc start frames).
  frames[15] = (void *)backtrace(frames, 15);
}

static void __attribute__((noinline)) level3(void **frames) { sink(frames); }

static void __attribute__((noinline)) level2(void **frames) { level3(frames); }

static void __attribute__((noinline)) level1(void **frames) { level2(frames); }

int main(void) {
  void *frames[16];
  level1(frames);

  int depth = (int)(intptr_t)frames[15];
  if (depth < 5) {
    printf("FAIL: backtrace returned only %d frames\n", depth);
    return 1;
  }

  char **syms = backtrace_symbols(frames, depth);
  if (!syms) {
    printf("FAIL: backtrace_symbols returned NULL\n");
    return 1;
  }
  for (int i = 0; i < depth; i++) {
    if (!syms[i] || !syms[i][0]) {
      printf("FAIL: empty symbol string for frame %d\n", i);
      return 1;
    }
  }
  free(syms);
  printf("PASS: %d frames\n", depth);
  return 0;
}
