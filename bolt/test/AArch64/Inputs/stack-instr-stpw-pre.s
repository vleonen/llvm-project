# Test: stp w0, w1, [sp, #-0x10]! (W-pair, immediate scale 4)
# Expected V=16 (allocate 16 bytes; the encoded immediate is -4)
.text
.global test_stpw_pre
.type test_stpw_pre,@function

test_stpw_pre:
    stp     w0, w1, [sp, #-0x10]!
    ldp     w0, w1, [sp], #0x10
    ret

.global _start
_start:
    bl test_stpw_pre
    ret
