/* Runs xc_bench and prints its report. As a ctest it is a smoke test: every
 * case must actually reach its HLT under both engines (a nonzero exit means
 * one did not, which would make the timings meaningless). The numbers
 * themselves are for humans -- on CI they say nothing, on a device they are
 * the first real performance data this project has. */
#include "xcore/bench.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    uint64_t iters = argc > 1 ? strtoull(argv[1], 0, 0) : 200000;
    char report[4096];
    int bad = xc_bench(report, sizeof report, iters);
    fputs(report, stdout);
    if (bad) printf("test_bench: %d case(s) failed to run\n", bad);
    return bad != 0;
}
