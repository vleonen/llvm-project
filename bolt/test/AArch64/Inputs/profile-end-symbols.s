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

  .align 12
  .globl foo_end
foo_end:
  .size foo_end, 0

  .align 12
  .globl bar_end
bar_end:
  .size bar_end, 0