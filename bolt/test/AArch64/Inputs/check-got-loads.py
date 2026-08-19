#!/usr/bin/env python3
"""Verify that ADRP+LDR GOT pairs reference the first two slots of the
output .got section. Usage: check-got-loads.py <got-addr-file> <disasm-file>
The disassembly must contain:
  adrp x0, <got-page>   / ldr x0, [x0, #<got-off>]
  adrp x1, <got-page>   / ldr x1, [x1, #<got-off + 8>]
"""
import sys

got_addr = int(open(sys.argv[1]).read().strip(), 16)
disasm = open(sys.argv[2]).read()

page = got_addr & ~0xFFF
off1 = got_addr & 0xFFF
off2 = off1 + 8

want = [
    f"adrp\tx0, 0x{page:x}",
    f"ldr\tx0, [x0, #0x{off1:x}]",
    f"adrp\tx1, 0x{page:x}",
    f"ldr\tx1, [x1, #0x{off2:x}]",
]
missing = [w for w in want if w not in disasm]
if missing:
    for w in missing:
        print(f"MISSING: {w}", file=sys.stderr)
    sys.exit(1)
print("GOT loads verified")
