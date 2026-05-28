/* Minimal dependency-free test harness for the C model. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <stdint.h>

static int g_tests = 0;
static int g_fails = 0;

#define CHECK(cond, ...) do {                              \
    g_tests++;                                             \
    if (!(cond)) {                                         \
        g_fails++;                                         \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);      \
        printf(__VA_ARGS__);                               \
        printf("\n");                                      \
    }                                                      \
} while (0)

#define CHECK_EQ_U(a, b, label) do {                                        \
    unsigned long _va = (unsigned long)(a), _vb = (unsigned long)(b);       \
    g_tests++;                                                              \
    if (_va != _vb) {                                                       \
        g_fails++;                                                          \
        printf("  FAIL %s:%d: %s expected 0x%lX got 0x%lX\n",              \
               __FILE__, __LINE__, (label), _vb, _va);                      \
    }                                                                      \
} while (0)

#define TEST_SUMMARY() do {                                                \
    printf("%s: %d/%d checks passed\n", __FILE__, g_tests - g_fails, g_tests); \
    return g_fails ? 1 : 0;                                                 \
} while (0)

#endif /* TEST_UTIL_H */
