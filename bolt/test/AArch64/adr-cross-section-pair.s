## Regression test for the cross-section ADR+LDR pair re-anchoring: GNU ld
## may relax an ADRP+LDR(GOT) pair into ADR+LDR, leaving the ADR targeting a
## page-aligned address whose loaded word (page + low-12 offset) lives in the
## NEXT section. When -rewrite moves the two sections apart, remapping the
## pair through the page's section produces a wrong address; the pass must
## re-anchor both instructions to a symbol at the actually loaded address.
## The binary loads the word through the pair and exits non-zero on mismatch.

# REQUIRES: system-linux,target=aarch64{{.*}}

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux-gnu %s -o %t.o
# RUN: %clang %cflags -nostdlib -static -Wl,-q %t.o -o %t.orig
# RUN: llvm-bolt %t.orig -o %t.bolt -rewrite
# RUN: %t.bolt
# Also verify the non-rewrite mode keeps the pair working.

  .text
  .globl _start
_start:
  adr   x8, dat           // page-aligned target (relaxed ADR)
  ldr   x9, [x8, #0xff8]  // word lives in the FOLLOWING section
  cmp   x9, #42
  b.ne  1f
  mov   w0, #0
  mov   w8, #93
  svc   #0
1:
  mov   w0, #1
  mov   w8, #93
  svc   #0

  // dat sits at the start of a page-aligned section; the loaded word is
  // 0xff8 past dat and lives in the FOLLOWING section (in a
  // the -rewrite layout places page-phase-aligned independently of this
  // section, so the pair must be re-anchored to the loaded address).
  .section .cross_sec1,"aw",@progbits
  .balign 4096
  .globl dat
dat:
  .zero 0x800

  .section .cross_sec2,"aw",@progbits
  .balign 2048
  .globl answer
  .zero 0x7f8
answer:
  .xword 42
