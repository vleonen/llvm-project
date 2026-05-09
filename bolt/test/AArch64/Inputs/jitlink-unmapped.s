.text
  .align 4
.globl _start
_start:
  ret
.size _start, .-_start

.data
  .align 4
dummy_data:
  .word 0
.size dummy_data, 4

.align 12
.globl unmapped_end
.type unmapped_end, @object
unmapped_end:
.size unmapped_end, 0
