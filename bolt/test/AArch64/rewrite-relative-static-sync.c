// Test -rewrite on a PIE AArch64 binary: the static file image must stay
// consistent with the R_AARCH64_RELATIVE relocations after rewriting.
// The linker produces each RELATIVE entry with the static bytes equal to
// the addend; any non-relocation-aware tool reading the file image relies
// on that canonical form (the dynamic loader re-applies the addends at
// load time, so the binary itself always runs correctly either way).
//
// Inputs/check-relative-static.py verifies the form externally - for every
// RELATIVE entry it compares the file bytes at r_offset against r_addend -
// so the test needs no execution. The table below is built with file-scope
// asm so that every entry is a static initializer: an exact function start,
// a .text interior pointer (func+8) and an interior pointer into .rodata.
//
// Built with bfd: unlike lld (which leaves the static bytes zero and
// relies solely on the dynamic relocations), bfd writes the addend into
// the section data, so the original binary satisfies the canonical
// static == addend form the check asserts.
//
// REQUIRES: system-linux, native, target=aarch64{{.*}}
//
// RUN: %clang %cflags -O2 -fuse-ld=bfd -Wl,--emit-relocs %s -o %t.orig
// RUN: bash -c 'chk() { local Bin=$1 n=0; read RS SZ < <(llvm-readelf -SW "$Bin" | awk "{for(i=1;i<NF;i++) if(\$i ~ /^\.data\.rel\.ro\$/){print \$(i+2), \$(i+4); exit}}"); [ -n "$RS" ] || { echo "no .data.rel.ro section"; exit 1; }; while read off add; do [ $((0x$off)) -ge $((0x$RS)) ] && [ $((0x$off)) -lt $((0x$RS + 0x$SZ)) ] || continue; want=$(printf "%016x" $((0x$add))); L=$(printf %x $(( 0x$RS + ((0x$off - 0x$RS) / 16) * 16 ))); H=$(( (0x$off - 0x$L) / 8 )); got=$(llvm-objdump -s -j .data.rel.ro "$Bin" | awk -v L=$L -v H=$H "function rv(s, i, r) { r = \"\"; for (i = 7; i >= 1; i -= 2) r = r substr(s, i, 2); return r } \$1 == L { if (H == 0) print rv(\$3) rv(\$2); else print rv(\$5) rv(\$4); exit }"); [ "$got" = "$want" ] || { echo "MISMATCH at $off: $got != $want"; exit 1; }; n=$((n+1)); done < <(llvm-readelf -rW "$Bin" | awk "/R_AARCH64_RELATIVE/{print \$1, \$NF}"); [ $n -gt 0 ] || { echo "no RELATIVE entries in .data.rel.ro"; exit 1; }; echo "relative static sync: ok ($n entries)"; }; chk %t.orig' | FileCheck %s --check-prefix=ORIG
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: bash -c 'chk() { local Bin=$1 n=0; read RS SZ < <(llvm-readelf -SW "$Bin" | awk "{for(i=1;i<NF;i++) if(\$i ~ /^\.data\.rel\.ro\$/){print \$(i+2), \$(i+4); exit}}"); [ -n "$RS" ] || { echo "no .data.rel.ro section"; exit 1; }; while read off add; do [ $((0x$off)) -ge $((0x$RS)) ] && [ $((0x$off)) -lt $((0x$RS + 0x$SZ)) ] || continue; want=$(printf "%016x" $((0x$add))); L=$(printf %x $(( 0x$RS + ((0x$off - 0x$RS) / 16) * 16 ))); H=$(( (0x$off - 0x$L) / 8 )); got=$(llvm-objdump -s -j .data.rel.ro "$Bin" | awk -v L=$L -v H=$H "function rv(s, i, r) { r = \"\"; for (i = 7; i >= 1; i -= 2) r = r substr(s, i, 2); return r } \$1 == L { if (H == 0) print rv(\$3) rv(\$2); else print rv(\$5) rv(\$4); exit }"); [ "$got" = "$want" ] || { echo "MISMATCH at $off: $got != $want"; exit 1; }; n=$((n+1)); done < <(llvm-readelf -rW "$Bin" | awk "/R_AARCH64_RELATIVE/{print \$1, \$NF}"); [ $n -gt 0 ] || { echo "no RELATIVE entries in .data.rel.ro"; exit 1; }; echo "relative static sync: ok ($n entries)"; }; chk %t.bolt' | FileCheck %s
//
// ORIG: relative static sync: ok
// CHECK: relative static sync: ok

#include <stdint.h>
int add3(int x) {
  volatile int y = x;
  y += 1;
  y += 1;
  y += 1;
  return y;
}

const int rodata_tab[256] = {1, 2, 3, [32] = 1337, [255] = 42};

__asm__(".section .data.rel.ro,\"aw\"\n"
        ".balign 8\n"
        ".globl relptr_tab\n"
        "relptr_tab:\n"
        "  .quad add3\n"
        "  .quad add3+8\n"
        "  .quad rodata_tab+128\n"
        ".size relptr_tab, 3*8\n"
        ".previous\n");

extern const void *const relptr_tab[3];

// The binary itself does not need to run: Inputs/check-relative-static.py
// verifies the file image externally. A freestanding _start keeps the
// link working under the AArch64 test dir's -nostdlib flags; reference
// the table so it cannot be eliminated.
void _start(void) {
  const void *volatile keep = relptr_tab[0];
  (void)keep;
}
