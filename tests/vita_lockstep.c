/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's lockstep settings (vita/runtime/lockstep.c, main.c): a build without -DSNX_VITA_LOCKSTEP=ON says once
   that it checks nothing, with the vita_lockstep given, and the variables an older version read are still
   vita_lockstep and vita_lockstep_steps, under the command line's own, each said with the value in effect */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv */
#endif
#include "vita/runtime/lockstep.h"
#include "vita/runtime/main.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int failures;
    char said[4096]; /* snx_error's lines */
} g_test;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\nsaid:\n%s", __FILE__, __LINE__, #x, g_test.said);                              \
            ++g_test.failures;                                                                                         \
        }                                                                                                              \
    } while (0)
static void said(const char* line) {
    size_t n = strlen(g_test.said);
    snprintf(g_test.said + n, sizeof g_test.said - n, "%s\n", line);
}
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
/* the driver with no game, which stops at its usage once the command line and the variables are read */
static void driver(const char* argument) {
    char program[] = "scarletnx-vita", given[64];
    char* argv[] = {program, given, NULL};
    snprintf(given, sizeof given, "%s", argument ? argument : "");
    g_test.said[0] = 0;
    g_settings.vita_lockstep = -1;
    g_settings.vita_lockstep_steps = 0;
    CHECK(snx_vita_main(argument ? 2 : 1, argv) == 2);
}

int main(void) {
    snx_error_sink(said);

    g_settings.vita_lockstep = 7;
    CHECK(vita_lockstep_step(NULL, NULL) == VITA_LOCKSTEP_OFF);
    CHECK(!strcmp(g_test.said,
                  "lockstep: vita_lockstep=7 needs a build configured with -DSNX_VITA_LOCKSTEP=ON: this run "
                  "checks nothing against Unicorn (docs/VITA.md)\n"));
    g_test.said[0] = 0;
    CHECK(vita_lockstep_step(NULL, NULL) == VITA_LOCKSTEP_OFF && !g_test.said[0]); /* once a run */

    set_env("VITA_LOCKSTEP", "12");
    set_env("VITA_LOCKSTEP_STEPS", "300");
    driver(NULL);
    CHECK(g_settings.vita_lockstep == 12 && g_settings.vita_lockstep_steps == 300);
    CHECK(strstr(g_test.said,
                 "VITA_LOCKSTEP=12 is deprecated, and this run reads it as --vita_lockstep: vita_lockstep=12 "
                 "(docs/DEPRECATIONS.md)\n"));
    CHECK(strstr(g_test.said, "VITA_LOCKSTEP_STEPS=300 is deprecated, and this run reads it as --vita_lockstep_steps: "
                              "vita_lockstep_steps=300 (docs/DEPRECATIONS.md)\n"));

    driver("--vita_lockstep_steps=40"); /* the command line's own comes first */
    CHECK(g_settings.vita_lockstep == 12 && g_settings.vita_lockstep_steps == 40);
    CHECK(strstr(g_test.said, "VITA_LOCKSTEP_STEPS=300 is deprecated, and the command line gives vita_lockstep_steps "
                              "first: vita_lockstep_steps=40 (docs/DEPRECATIONS.md)\n"));

    set_env("VITA_LOCKSTEP_STEPS", "0"); /* compared no group, where vita_lockstep_steps=0 has no limit */
    driver(NULL);
    CHECK(g_settings.vita_lockstep == -1 && g_settings.vita_lockstep_steps == 0);
    CHECK(strstr(g_test.said,
                 "VITA_LOCKSTEP=12 is deprecated, and VITA_LOCKSTEP_STEPS=0 compared none: vita_lockstep=-1"));
    CHECK(strstr(g_test.said,
                 "VITA_LOCKSTEP_STEPS=0 is deprecated, and compared no instruction group, which "
                 "vita_lockstep_steps=0 does not mean (no limit): vita_lockstep=-1 (docs/DEPRECATIONS.md)\n"));

    set_env("VITA_LOCKSTEP", NULL);
    set_env("VITA_LOCKSTEP_STEPS", NULL);
    driver(NULL);
    CHECK(!strstr(g_test.said, "VITA_LOCKSTEP"));
    printf("vita_lockstep: %s\n",
           g_test.failures ? "FAILED" : "the build without Unicorn says so, the old variables still read");
    return g_test.failures != 0;
}
