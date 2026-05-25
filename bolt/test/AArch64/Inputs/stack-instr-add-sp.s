# Test: add sp, sp, #0x80
# Expected V=-128 (deallocate 128 bytes)
.text
.global test_add_sp
.type test_add_sp,@function

test_add_sp:
    sub     sp, sp, #0x80
    add     sp, sp, #0x80
    ret

.global _start
_start:
    bl test_add_sp
    ret