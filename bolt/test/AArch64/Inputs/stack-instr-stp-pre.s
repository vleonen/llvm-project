# Test: stp x0, x1, [sp, #-0x10]!
# Expected V=16 (allocate 16 bytes)
.text
.global test_stp_pre
.type test_stp_pre,@function

test_stp_pre:
    stp     x0, x1, [sp, #-0x10]!
    ldp     x0, x1, [sp], #0x10
    ret

.global _start
_start:
    bl test_stp_pre
    ret