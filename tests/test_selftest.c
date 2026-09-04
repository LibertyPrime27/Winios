/* Runs the golden-vector replay as a normal test, so a divergence between the
 * interpreter and the recorded silicon post-states fails CI too -- not only on
 * device where it would be much harder to notice. */
#include "xcore/golden.h"
#include <stdio.h>

int main(void) {
    static char report[65536];
    int bad = xc_selftest(report, sizeof report, 400);
    fputs(report, stdout);
    if (bad) { fprintf(stderr, "selftest: %d mismatches\n", bad); return 1; }
    return 0;
}
