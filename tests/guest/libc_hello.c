/* The second guest: a real C library (musl, static). printf brings in
 * vfprintf, malloc, memcpy, strlen, the SSE2 string routines, TLS setup via
 * arch_prctl, and a dozen more syscalls -- exactly what Wine's own startup
 * needs before it does anything interesting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    char *s = malloc(64);
    snprintf(s, 64, "argc=%d argv0=%s", argc, argv[0]);
    printf("%s len=%zu\n", s, strlen(s));
    unsigned long h = 1469598103934665603ull;
    for (int i = 0; i < 1000; i++) h = (h ^ (unsigned long)i) * 1099511628211ull;
    printf("fnv=%lx\n", h);
    return argc == 1 ? 0 : 3;
}
