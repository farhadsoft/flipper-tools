#pragma once

// Minimal single-header test framework. No fixtures, no discovery magic:
// list cases explicitly in main() via RUN_TEST(). Intended for one .c file
// including this header, so the file-scope statics below don't collide.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_test_count = 0;
static int g_test_failures = 0;

static inline void test_print_hex(const char* label, const uint8_t* data, size_t len) {
    printf("%s: ", label);
    for(size_t i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

#define TEST_CASE(name) static void name(void)

#define RUN_TEST(name)                 \
    do {                               \
        printf("--- %s ---\n", #name); \
        name();                        \
    } while(0)

#define ASSERT_TRUE(cond)                                                       \
    do {                                                                        \
        g_test_count++;                                                        \
        if(!(cond)) {                                                           \
            g_test_failures++;                                                 \
            printf("FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while(0)

#define ASSERT_BYTES_EQ(expected, actual, len)                               \
    do {                                                                     \
        g_test_count++;                                                     \
        if(memcmp((expected), (actual), (len)) != 0) {                      \
            g_test_failures++;                                              \
            printf(                                                         \
                "FAIL %s:%d: ASSERT_BYTES_EQ(%s, %s)\n",                    \
                __FILE__,                                                   \
                __LINE__,                                                   \
                #expected,                                                  \
                #actual);                                                   \
            test_print_hex("  expected", (const uint8_t*)(expected), (len)); \
            test_print_hex("  actual  ", (const uint8_t*)(actual), (len));   \
        }                                                                    \
    } while(0)

// Prints the run summary and returns the process exit code (0 = all passed).
static inline int test_report(void) {
    printf("\n%d assertions, %d failed\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
