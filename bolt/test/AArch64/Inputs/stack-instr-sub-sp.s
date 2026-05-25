# Test: sub sp, sp, #0x80
# Expected V=128 (allocate 128 bytes)
.text
.global test_sub_sp
.type test_sub_sp,@function

test_sub_sp:
    sub     sp, sp, #0x80
    add     sp, sp, #0x80
    ret

.global _start
_start:
    bl test_sub_sp
    ret