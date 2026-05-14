## Test that BOLT correctly handles multi-byte NOP instructions with --keep-nops.
## This tests the createNoopSize() function for sizes 1-15 bytes.
## NOPs > 10 bytes are split into multiple instructions.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q
# RUN: llvm-bolt %t.exe -o %t.bolt.exe --keep-nops --relocs --print-finalized \
# RUN:   |& FileCheck --check-prefix=CHECK-BOLT %s
# RUN: llvm-objdump -d %t.bolt.exe | FileCheck --check-prefix=CHECK-OBJ %s

  .text
  .globl _start
  .type _start,@function
_start:
  .cfi_startproc

  .byte 0x90                                        # 1-byte NOP
  .byte 0x66, 0x90                                  # 2-byte NOP
  .byte 0x0f, 0x1f, 0x00                            # 3-byte NOP: nopl (%rax)
  .byte 0x0f, 0x1f, 0x40, 0x00                      # 4-byte NOP: nopl 0(%rax)
  .byte 0x0f, 0x1f, 0x44, 0x00, 0x00                # 5-byte NOP: nopl 0(%rax,%rax,1)
  .byte 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00          # 6-byte NOP: nopw 0(%rax,%rax,1)
  .byte 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00    # 7-byte NOP: nopl 0L(%rax)
  .byte 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 8-byte NOP: nopl 0L(%rax,%rax,1)
  .byte 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 9-byte NOP: nopw 0L(%rax,%rax,1)
  .byte 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 10-byte NOP
  .byte 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 11-byte NOP (10+1)
  .byte 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 12-byte NOP (10+2)
  .byte 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 13-byte NOP (10+3)
  .byte 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 14-byte NOP (10+4)
  .byte 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  # 15-byte NOP (10+5)

## Sizes 1-10: single instruction each
# CHECK-BOLT:      nop # Size: 1
# CHECK-BOLT-NEXT: xchgw	%rax, %ax # Size: 2
# CHECK-BOLT-NEXT: nop # Size: 3
# CHECK-BOLT-NEXT: {disp8}	nop # Size: 4
# CHECK-BOLT-NEXT: {disp8}	nop # Size: 5
# CHECK-BOLT-NEXT: {disp8}	nop # Size: 6
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 7
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 8
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 9
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
## Sizes 11-15: split into 10 + remainder
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
# CHECK-BOLT-NEXT: nop # Size: 1
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
# CHECK-BOLT-NEXT: xchgw	%rax, %ax # Size: 2
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
# CHECK-BOLT-NEXT: nop # Size: 3
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
# CHECK-BOLT-NEXT: {disp8}	nop # Size: 4
# CHECK-BOLT-NEXT: {disp32}	nop # Size: 10
# CHECK-BOLT-NEXT: {disp8}	nop # Size: 5

# CHECK-OBJ:      Disassembly of section .text:
# CHECK-OBJ-EMPTY:
# CHECK-OBJ-NEXT: 0000000000600000 <_start>:
# CHECK-OBJ-NEXT:  600000: 90
# CHECK-OBJ-NEXT:  600001: 66 90
# CHECK-OBJ-NEXT:  600003: 0f 1f 00
# CHECK-OBJ-NEXT:  600006: 0f 1f 40 00
# CHECK-OBJ-NEXT:  60000a: 0f 1f 44 00 00
# CHECK-OBJ-NEXT:  60000f: 66 0f 1f 44 00 00
# CHECK-OBJ-NEXT:  600015: 0f 1f 80 00 00 00 00
# CHECK-OBJ-NEXT:  60001c: 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  600024: 66 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  60002d: 66 2e 0f 1f 84 00 00 00 00 00
# 11-byte NOP split into 10-byte + 1-byte
# CHECK-OBJ-NEXT:  600037: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  600041: 90
# 12-byte NOP split into 10-byte + 2-byte
# CHECK-OBJ-NEXT:  600042: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  60004c: 66 90
# 13-byte NOP split into 10-byte + 3-byte
# CHECK-OBJ-NEXT:  60004e: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  600058: 0f 1f 00
# 14-byte NOP split into 10-byte + 4-byte
# CHECK-OBJ-NEXT:  60005b: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  600065: 0f 1f 40 00
# 15-byte NOP split into 10-byte + 5-byte
# CHECK-OBJ-NEXT:  600069: 66 2e 0f 1f 84 00 00 00 00 00
# CHECK-OBJ-NEXT:  600073: 0f 1f 44 00 00

  .reloc 0, R_X86_64_NONE

  .size _start, .-_start
  .cfi_endproc