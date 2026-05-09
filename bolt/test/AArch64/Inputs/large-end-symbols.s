.text
  .align 4
  .globl _start
  .type _start, @function
_start:
  ret
  .size _start, .-_start

  .space 134217728

  .align 12
  .globl etext
  .type etext, @object
etext:
  .size etext, 0