// Runtime test that verifies EndSymbols work correctly with JITLink
// when the binary is actually executed.
//
// REQUIRES: system-linux, target=aarch64{{.*}}, asserts
//
// RUN: %clang %cflags -no-pie %s -o %t.exe
// RUN: %t.exe
// RUN: llvm-bolt %t.exe -o %t.bolt --debug-only=bolt 2>&1 | FileCheck %s
// RUN: %t.bolt
//
// CHECK: etext is in the end of .
// CHECK: _end is in the end of .bss

extern char _end;
extern char etext;

int main(void) {
  // The end symbols must remain accessible and point at distinct regions
  // of the rewritten binary.
  volatile char *end_ptr = &_end;
  volatile char *etext_ptr = &etext;
  return (end_ptr == etext_ptr) ? 1 : 0;
}
