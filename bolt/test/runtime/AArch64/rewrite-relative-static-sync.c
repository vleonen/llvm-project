// Test -rewrite on a PIE AArch64 binary: the static file image must stay
// consistent with the R_AARCH64_RELATIVE relocations after rewriting.
// The linker produces each RELATIVE entry with the static bytes equal to
// the addend; any non-relocation-aware tool reading the file image relies
// on that canonical form (the dynamic loader re-applies the addends at
// load time, so the binary itself always runs correctly either way).
//
// The binary self-checks its own file image: it opens /proc/self/exe,
// walks PT_DYNAMIC to find .rela.dyn and, for every R_AARCH64_RELATIVE
// entry targeting the .data.rel.ro pointer table below, compares the
// static bytes at r_offset against r_addend. The table is built with
// file-scope asm so that every entry is a static initializer: an exact
// function start, a .text interior pointer (func+8) and an interior
// pointer into .rodata.
//
// Built with bfd: unlike lld (which leaves the static bytes zero and
// relies solely on the dynamic relocations), bfd writes the addend into
// the section data, so the original binary satisfies the canonical
// static == addend form the check asserts.
//
// REQUIRES: system-linux
//
// RUN: %clang %cflags -O2 -fuse-ld=bfd -Wl,--emit-relocs %s -o %t.orig
// RUN: %t.orig | FileCheck %s --check-prefix=ORIG
// RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
// RUN: %t.bolt | FileCheck %s
//
// ORIG: relative static sync: ok
// CHECK: relative static sync: ok
// CHECK-NOT: FAIL

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <sys/auxv.h>

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

static uint64_t read64le(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | p[i];
  return v;
}

// Map a virtual address to a file offset through the PT_LOAD segments.
static int64_t v2f(const Elf64_Ehdr *eh, uint64_t v) {
  const Elf64_Phdr *ph =
      (const Elf64_Phdr *)((const unsigned char *)eh + eh->e_phoff);
  for (unsigned i = 0; i < eh->e_phnum; ++i)
    if (ph[i].p_type == PT_LOAD && v >= ph[i].p_vaddr &&
        v < ph[i].p_vaddr + ph[i].p_filesz)
      return (int64_t)(ph[i].p_offset + (v - ph[i].p_vaddr));
  return -1;
}

int main() {
  // Exercise the relocated pointers so the loader-applied values are
  // actually used and nothing is eliminated.
  volatile int res = 0;
  int (*const volatile f)(int) = (int (*)(int))relptr_tab[0];
  const int *const volatile rp = (const int *)relptr_tab[2];
  res += f(1);
  res += *rp;
  (void)res;

  // Read the own file image.
  int fd = open("/proc/self/exe", O_RDONLY);
  if (fd < 0) {
    puts("FAIL: cannot open /proc/self/exe");
    return 1;
  }
  off_t fsize = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  unsigned char *img = malloc(fsize);
  if (!img || read(fd, img, fsize) != fsize) {
    puts("FAIL: cannot read file image");
    return 1;
  }
  close(fd);
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
    puts("FAIL: not an ELF file");
    return 1;
  }

  // Locate .rela.dyn through PT_DYNAMIC.
  uint64_t rela = 0, relasz = 0;
  const Elf64_Phdr *ph =
      (const Elf64_Phdr *)((const unsigned char *)eh + eh->e_phoff);
  for (unsigned i = 0; i < eh->e_phnum; ++i) {
    if (ph[i].p_type != PT_DYNAMIC)
      continue;
    int64_t d = v2f(eh, ph[i].p_vaddr);
    if (d < 0)
      continue;
    for (const Elf64_Dyn *dyn = (const Elf64_Dyn *)(img + d);
         dyn->d_tag != DT_NULL; ++dyn) {
      if (dyn->d_tag == DT_RELA)
        rela = dyn->d_un.d_ptr;
      else if (dyn->d_tag == DT_RELASZ)
        relasz = dyn->d_un.d_val;
    }
  }
  int64_t rf = (rela && relasz) ? v2f(eh, rela) : -1;
  if (rf < 0 || rf + (int64_t)relasz > fsize) {
    puts("FAIL: no usable DT_RELA table");
    return 1;
  }

  // Every R_AARCH64_RELATIVE entry targeting the pointer table must have
  // the static bytes at r_offset equal to r_addend. The runtime address of
  // the table includes the load bias; relocation offsets are link-time
  // virtual addresses.
  const uint64_t bias = getauxval(AT_PHDR) - eh->e_phoff;
  const uint64_t tab = (uint64_t)&relptr_tab[0] - bias;
  int hits = 0;
  int failures = 0;
  for (uint64_t o = 0; o + sizeof(Elf64_Rela) <= relasz;
       o += sizeof(Elf64_Rela)) {
    const Elf64_Rela *r = (const Elf64_Rela *)(img + rf + o);
    if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE)
      continue;
    if (r->r_offset < tab || r->r_offset >= tab + 3 * 8)
      continue;
    ++hits;
    int64_t f = v2f(eh, r->r_offset);
    uint64_t statik = (f >= 0 && f + 8 <= fsize) ? read64le(img + f) : 0;
    if (statik != (uint64_t)r->r_addend) {
      printf("FAIL: RELATIVE at %#lx: static %#lx != addend %#lx\n",
             (long)r->r_offset, (long)statik, (long)r->r_addend);
      ++failures;
    }
  }
  if (hits != 3) {
    printf("FAIL: expected 3 RELATIVE entries on the table, found %d\n",
           hits);
    ++failures;
  }
  if (failures)
    return 1;
  printf("relative static sync: ok (%d entries checked)\n", hits);
  return 0;
}
