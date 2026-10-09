#include <stdio.h>

int read_tls(void);

int main(void) {
  printf("tlsdesc ok %d\n", read_tls());
  return 0;
}
