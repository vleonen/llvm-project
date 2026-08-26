// Runtime test that -rewrite mode keeps TLS working. This is the
// worst case for PT_TLS rebuilding: with crt files present, NOBITS .tbss
// shares its vaddr range with the following .init_array, and a naive
// first-match section lookup builds the new descriptor from the wrong
// section, corrupting the TLS template. The test verifies the PT_TLS
// descriptor sizes and executes the rewritten binary to check that
// thread-local variables in .tdata and .tbss are accessible.

#include <stdio.h>

__thread int tdata_var = 42; // .tdata
__thread int tbss_var;       // .tbss

int main() {
  printf("%d %d\n", tdata_var, ++tbss_var);
  return 0;
}

// REQUIRES: system-linux

// RUN: %clang %cflags -fPIC -pie %s -o %t.exe -Wl,-q -fuse-ld=lld
// RUN: llvm-readelf -lW %t.exe | FileCheck %s --check-prefix=TLS-IN
// RUN: llvm-bolt %t.exe -o %t.rw -rewrite
// RUN: llvm-readelf -lW %t.rw | FileCheck %s --check-prefix=TLS-OUT
// RUN: %t.rw | FileCheck %s --check-prefix=OUT

// TLS-IN: TLS 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x000004 0x000008 R
// TLS-OUT: TLS 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x000004 0x000008 R

// OUT: 42 1
