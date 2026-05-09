// Test input for JITLink integration verification
// Binary with multiple section-end symbols for comprehensive testing

  .text
  .align 4
  .globl _start
  .type _start,@function
_start:
  ret
  .size _start, .-_start

.align 12
.globl text_end
.type text_end, @object
text_end:
.size text_end, 0

  .data  
  .align 4
glob_data:
  .word 12345
  .size glob_data, 4

.align 12
.globl data_end
.type data_end, @object
data_end:
.size data_end, 0