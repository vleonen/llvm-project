.text
  .align 4
  .globl _start
  .type _start, @function
_start:
  adrp x0, etext
  add x0, x0, :lo12:etext
  ret
  .size _start, .-_start

  .align 12
  .globl etext
  .type etext, @object
etext:
  .size etext, 0