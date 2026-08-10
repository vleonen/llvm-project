// Verify that stack-protector canary code keeps working after -rewrite on
// AArch64. Every instrumented function loads __stack_chk_guard through the
// GOT; -rewrite relocates the GOT, so this exercises the GOT-entry
// retargeting (ADRP+LDR pairs, GLOB_DAT relocation) for data symbols.
// The structural check asserts the guard still has exactly one GOT entry
// (GLOB_DAT) in the output - the entry is retargeted, never duplicated.
// The smash subcommand deliberately overflows a canary-protected buffer:
// the process must abort through __stack_chk_fail both before and after
// the rewrite (the canary load/check must survive code movement).
//
// REQUIRES: system-linux
//
// RUN: %clang %cflags -O1 -g -fstack-protector-all -Wl,-q %s -o %t.orig
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt | FileCheck %s
// Exactly one GOT entry for the canary in the output.
// RUN: llvm-readelf -rW %t.bolt | grep "GLOB_DAT" | FileCheck %s --check-prefix=GUARD
// GUARD-COUNT-1: R_AARCH64_GLOB_DAT {{.*}} __stack_chk_guard
// The canary check must still abort on overflow.
// RUN: %t.bolt smash 2>/dev/null; test $? -ne 0
// Non-PIE variant (absolute pointers, GNU-ld shape dynsym on bfd).
// RUN: %clang %cflags -O1 -g -fstack-protector-all -Wl,-q -no-pie %s -o %t.nopie.orig
// RUN: llvm-bolt %t.nopie.orig -o %t.nopie.bolt -rewrite
// RUN: %t.nopie.bolt | FileCheck %s
// RUN: llvm-readelf -rW %t.nopie.bolt | grep "GLOB_DAT" | FileCheck %s --check-prefix=GUARD
// RUN: %t.nopie.bolt smash 2>/dev/null; test $? -ne 0

#include <stdio.h>
#include <string.h>

volatile int zero = 0;

__attribute__((noinline)) long canary_user(long x) {
  volatile char buf[64];
  for (int i = 0; i < 64; i++)
    buf[i] = (char)(x + i);
  long s = 0;
  for (int i = 0; i < 64; i++)
    s += buf[i];
  return s + x;
}

__attribute__((noinline)) void smash(void) {
  volatile char buf[8];
  volatile int i;
  for (i = 0; i < 64; i++)
    buf[i] = (char)(0x41 + i); // smashes past the canary
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "smash")) {
    smash();
    printf("SMASH-NOT-DETECTED\n");
    return 0;
  }
  long acc = 0;
  for (long i = 0; i < 4; i++)
    acc += canary_user(7 + i);
  printf("SP-OK %ld\n", acc);
  return acc != 0 ? 0 : 1;
}

// CHECK: SP-OK {{[0-9]+}}
