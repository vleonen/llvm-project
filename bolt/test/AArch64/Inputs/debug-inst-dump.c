// Dense switch with noinline case bodies: forces the compiler to emit a
// jump table (adr/adrp + ldr + br chain) so that BOLT's indirect-branch
// analysis walks the instructions and dumps them under -debug.
volatile int seed = 15;
__attribute__((noinline)) int c(int n) { return n; }
__attribute__((noinline)) int c0(int n) { return n * 1 + 0; }
__attribute__((noinline)) int c1(int n) { return n * 2 + 1; }
__attribute__((noinline)) int c2(int n) { return n * 3 + 2; }
__attribute__((noinline)) int c3(int n) { return n * 4 + 3; }
__attribute__((noinline)) int c4(int n) { return n * 5 + 4; }
__attribute__((noinline)) int c5(int n) { return n * 6 + 5; }
__attribute__((noinline)) int c6(int n) { return n * 7 + 6; }
__attribute__((noinline)) int c7(int n) { return n * 8 + 7; }
__attribute__((noinline)) int c8(int n) { return n * 9 + 8; }
__attribute__((noinline)) int c9(int n) { return n * 10 + 9; }
__attribute__((noinline)) int c10(int n) { return n * 11 + 10; }
__attribute__((noinline)) int c11(int n) { return n * 12 + 11; }
__attribute__((noinline)) int c12(int n) { return n * 13 + 12; }
__attribute__((noinline)) int c13(int n) { return n * 14 + 13; }
__attribute__((noinline)) int c14(int n) { return n * 15 + 14; }
__attribute__((noinline)) int c15(int n) { return n * 16 + 15; }
__attribute__((noinline)) int c16(int n) { return n * 17 + 16; }
__attribute__((noinline)) int c17(int n) { return n * 18 + 17; }
__attribute__((noinline)) int c18(int n) { return n * 19 + 18; }
__attribute__((noinline)) int c19(int n) { return n * 20 + 19; }
__attribute__((noinline)) int c20(int n) { return n * 21 + 20; }
__attribute__((noinline)) int c21(int n) { return n * 22 + 21; }
__attribute__((noinline)) int c22(int n) { return n * 23 + 22; }
__attribute__((noinline)) int c23(int n) { return n * 24 + 23; }
__attribute__((noinline)) int c24(int n) { return n * 25 + 24; }
__attribute__((noinline)) int c25(int n) { return n * 26 + 25; }
__attribute__((noinline)) int c26(int n) { return n * 27 + 26; }
__attribute__((noinline)) int c27(int n) { return n * 28 + 27; }
__attribute__((noinline)) int c28(int n) { return n * 29 + 28; }
__attribute__((noinline)) int c29(int n) { return n * 30 + 29; }

__attribute__((noinline)) int id(int x, int n) {
  switch (x & 31) {
  case 0: return c0(n);
  case 1: return c1(n);
  case 2: return c2(n);
  case 3: return c3(n);
  case 4: return c4(n);
  case 5: return c5(n);
  case 6: return c6(n);
  case 7: return c7(n);
  case 8: return c8(n);
  case 9: return c9(n);
  case 10: return c10(n);
  case 11: return c11(n);
  case 12: return c12(n);
  case 13: return c13(n);
  case 14: return c14(n);
  case 15: return c15(n);
  case 16: return c16(n);
  case 17: return c17(n);
  case 18: return c18(n);
  case 19: return c19(n);
  case 20: return c20(n);
  case 21: return c21(n);
  case 22: return c22(n);
  case 23: return c23(n);
  case 24: return c24(n);
  case 25: return c25(n);
  case 26: return c26(n);
  case 27: return c27(n);
  case 28: return c28(n);
  case 29: return c29(n);
  }
  return c(n);
}
void _start(void) { id(seed, 2); }
