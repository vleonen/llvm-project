# Test: str x2, [sp, #-0x10]!
# Expected V=16 (allocate 16 bytes)
.text
.global test_str_pre
.type test_str_pre,@function

test_str_pre:
    str     x2, [sp, #-0x10]!
    ldr     x2, [sp], #0x10
    ret

.global _start
_start:
    bl test_str_pre
    ret