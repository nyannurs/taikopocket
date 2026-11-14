/* Minimal test helpers: CHECK() records failures, TEST_MAIN_END returns the status. */
#ifndef TP_TEST_H
#define TP_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long test_a = (long long)(a), test_b = (long long)(b);          \
        test_checks++;                                                       \
        if (test_a != test_b) {                                              \
            test_failures++;                                                 \
            printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, test_a, \
                   test_b);                                                  \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        double test_a = (double)(a), test_b = (double)(b);                   \
        test_checks++;                                                       \
        if (!(fabs(test_a - test_b) <= (eps))) {                             \
            test_failures++;                                                 \
            printf("  FAIL %s:%d: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, #a, test_a, \
                   test_b);                                                  \
        }                                                                    \
    } while (0)

#define TEST_MAIN_END                                                        \
    printf("  %d checks, %d failures\n", test_checks, test_failures);        \
    return test_failures ? 1 : 0

#endif
