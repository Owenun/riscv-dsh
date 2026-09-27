#ifndef RVSIM_TEST_FRAMEWORK_H
#define RVSIM_TEST_FRAMEWORK_H

#include <stdio.h>

static int rvsim_checks_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            printf("PASS %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        } else {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            rvsim_checks_failed++;                                           \
        }                                                                    \
    } while (0)

#define TEST_DONE()                                                          \
    do {                                                                     \
        if (rvsim_checks_failed != 0) {                                      \
            printf("%d check(s) failed\n", rvsim_checks_failed);             \
            return 1;                                                        \
        }                                                                    \
        return 0;                                                            \
    } while (0)

#endif
