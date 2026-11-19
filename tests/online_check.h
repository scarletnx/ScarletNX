/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The tests' checks: each failure says where and what, and the run's exit status counts them */
#ifndef SNX_ONLINE_TEST_CHECK_H
#define SNX_ONLINE_TEST_CHECK_H
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                   \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK_INT(got, want)                                                                                           \
    do {                                                                                                               \
        long long g_ = (long long)(got), w_ = (long long)(want);                                                       \
        if (g_ != w_) {                                                                                                \
            fprintf(stderr, "%s:%d: %s is %lld, not %lld\n", __FILE__, __LINE__, #got, g_, w_);                        \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        const char *g_ = (got), *w_ = (want);                                                                          \
        if (!g_ || strcmp(g_, w_)) {                                                                                   \
            fprintf(stderr, "%s:%d: %s is \"%s\", not \"%s\"\n", __FILE__, __LINE__, #got, g_ ? g_ : "(null)", w_);    \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
/* text holds part: a message names the value it is about */
#define CHECK_HAS(text, part)                                                                                          \
    do {                                                                                                               \
        if (!strstr((text), (part))) {                                                                                 \
            fprintf(stderr, "%s:%d: \"%s\" does not say \"%s\"\n", __FILE__, __LINE__, (text), (part));                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK_DONE()                                                                                                   \
    do {                                                                                                               \
        if (g_failures) fprintf(stderr, "%d checks failed\n", g_failures);                                             \
        return g_failures ? 1 : 0;                                                                                     \
    } while (0)

#endif
