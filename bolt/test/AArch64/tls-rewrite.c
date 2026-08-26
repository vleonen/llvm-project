// Test that -rewrite mode rebuilds PT_TLS correctly. NOBITS .tbss shares
// its vaddr range with the following file-backed sections, so a
// first-match section lookup for the new descriptor picks a wrong
// section and drops the .tbss part of the TLS template. The test checks
// that p_filesz covers only .tdata and p_memsz covers the full
// .tdata + .tbss span.

__thread int tdata_var = 42; // .tdata
__thread int tbss_var;       // .tbss

void _start() { tbss_var = tdata_var; }

// REQUIRES: system-linux
// RUN: %clang %cflags -fPIC -pie %s -o %t.exe -Wl,-q -fuse-ld=lld
// RUN: llvm-readelf -lW %t.exe | FileCheck %s --check-prefix=TLS-IN
// RUN: llvm-bolt %t.exe -o %t.rw -rewrite
// RUN: llvm-readelf -lW %t.rw | FileCheck %s --check-prefix=TLS-OUT

// TLS-IN: TLS 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x000004 0x000008 R
// TLS-OUT: TLS 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} 0x000004 0x000008 R
