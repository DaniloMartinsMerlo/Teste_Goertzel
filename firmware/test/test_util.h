#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int g_tests = 0;
static int g_fails = 0;

#define CHECK(cond, ...) do {                                            \
    g_tests++;                                                           \
    if (!(cond)) {                                                       \
        g_fails++;                                                       \
        printf("  FALHA %s:%d: ", __FILE__, __LINE__);                   \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

#define CHECK_NEAR(got, want, tol, ...) do {                             \
    double _g = (double)(got), _w = (double)(want), _t = (double)(tol);  \
    g_tests++;                                                           \
    if (fabs(_g - _w) > _t) {                                            \
        g_fails++;                                                       \
        printf("  FALHA %s:%d: got=%.6g want=%.6g tol=%.3g : ",          \
               __FILE__, __LINE__, _g, _w, _t);                          \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

#define SECTION(name) printf("\n== %s ==\n", (name))

static int test_report(const char *suite)
{
    printf("\n%s: %d checagens, %d falhas\n", suite, g_tests, g_fails);
    return g_fails == 0 ? 0 : 1;
}

static inline double db(double ratio)
{
    return 20.0 * log10(ratio > 1e-300 ? ratio : 1e-300);
}

#endif /* TEST_UTIL_H */
