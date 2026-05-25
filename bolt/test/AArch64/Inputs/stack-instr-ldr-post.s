# Test: ldr x2, [sp], #0x10
# Expected V=-16 (deallocate 16 bytes)
.text
.global test_ldr_post
.type test_ldr_post,@function

test_ldr_post:
    str     x2, [sp, #-0x10]!
    ldr     x2, [sp], #0x10
    ret

.global _start
_start:
    bl test_ldr_post
    ret