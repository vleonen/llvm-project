#include <stdio.h>

class ExcA {};
class ExcC {};

static void foo(int a) {
  if (a > 1)
    throw ExcA();
  else
    throw ExcC();
}

int main(int argc, char **argv) {
  asm volatile ("nop;nop;nop;nop;nop");
  try {
    try {
      asm volatile ("nop;nop;nop;nop;nop");
      throw ExcA();
    } catch (ExcA) {
      asm volatile ("nop;nop;nop;nop;nop");
      printf("catch 2\n");
      throw new int();
    }
  } catch (...) {
    asm volatile ("nop;nop;nop;nop;nop");
    printf("catch 1\n");
  }

  try {
    asm volatile ("nop;nop;nop;nop;nop");
    try {
      foo(argc);
    } catch (ExcC) {
      asm volatile ("nop;nop;nop;nop;nop");
      printf("caught ExcC\n");
    }
  } catch (ExcA) {
    printf("caught ExcA\n");
  }

  return 0;
}
