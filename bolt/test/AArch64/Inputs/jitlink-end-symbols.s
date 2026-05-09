.text
  .align 4
  .globl _start
.type _start, @function
_start:
  ret
  .size _start, .-_start

.align 12
.globl etext
.type etext, @object
etext:
.size etext, 0

.data
  .align 4
  .globl mydata
mydata:
  .word 42
  .size mydata, 4

.align 12
.globl _edata
.type _edata, @object
_edata:
.size _edata, 0

.bss
  .align 4
  .globl mybss
mybss:
  .space 8
  .size mybss, 8

.align 12
.globl _end
.type _end, @object
_end:
.size _end, 0
