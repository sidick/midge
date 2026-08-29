/* test.h — tiny host-side test harness for the portable core.
 *
 * A test uses TEST_CHECK(cond) for a real assertion. The runner exits
 * non-zero only on a real failure. */
#ifndef MIDGE_TEST_H
#define MIDGE_TEST_H

#include <stdio.h>

typedef struct {
    int passed;
    int failed;
} test_ctx;

extern test_ctx g_test;

#define TEST_CHECK(cond)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            g_test.passed++;                                                \
        } else {                                                            \
            g_test.failed++;                                                \
            fprintf(stderr, "FAIL    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#endif /* MIDGE_TEST_H */
