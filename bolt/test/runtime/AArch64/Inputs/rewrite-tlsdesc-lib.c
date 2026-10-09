__thread int tls_value = 41;

int read_tls(void) { return tls_value + 1; }
