void puts(const char *);

static void myinit(void) {}

// Manually place an absolute function pointer in .init_array: with -no-pie
// the linker resolves it at link time (no dynamic relocation), which is
// the class of pointers -rewrite must patch in the output. The puts() call
// goes through a PLT so that .symtab contains an STT_FUNC symbol resolving
// to an address inside the PLT section.
__attribute__((used, section(".init_array"))) static void (
    *const initp)(void) = myinit;

void _start(void) { puts("x"); }
