#include <stdio.h>

static void preinit_fn(void) { puts("preinit ran"); }
__attribute__((used, section(".preinit_array"))) static void (*const p)(void) =
    preinit_fn;

int main(void) {
  puts("main ran");
  return 0;
}
