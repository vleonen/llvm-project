# REQUIRES: system-linux

# -rewrite must reject combinations with options whose assumptions it breaks:
# --merge-text-sections (its merged-section layout interacts with the
# segment-based rewrite layout) and --relax-exp (the experimental relaxation
# pass has not been validated against PLT-as-functions emission).

# RUN: %clang %cflags -Wl,-q %s -o %t.exe
# RUN: not llvm-bolt %t.exe -o %t.bolt -rewrite --merge-text-sections 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CHECK-MTS
# CHECK-MTS: BOLT-ERROR: -rewrite is incompatible with --merge-text-sections

# RUN: not llvm-bolt %t.exe -o %t.bolt -rewrite --relax-exp 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CHECK-RE
# CHECK-RE: BOLT-ERROR: -rewrite is incompatible with --relax-exp

.globl _start
.text
_start:
.Lstart:
  lea .Lmsg(%rip), %rdi
  ret
.section .rodata,"a"
.Lmsg:
  .asciz "x"
.section .data,"aw"
quux:
  .quad .Lstart
