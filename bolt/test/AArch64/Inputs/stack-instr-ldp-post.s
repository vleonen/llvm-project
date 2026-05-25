# Test: ldp x0, x1, [sp], #0x10
# Expected V=-16 (deallocate 16 bytes)
.text
.global test_ldp_post
.type test_ldp_post,@function

test_ldp_post:
    stp     x0, x1, [sp, #-0x10]!
    ldp     x0, x1, [sp], #0x10
    ret

.global _start
_start:
    bl test_ldp_post
    ret