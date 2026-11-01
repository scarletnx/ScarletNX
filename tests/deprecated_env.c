/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The environment variables an older runtime read (nx/runtime/deprecated_env.c): each one set is this run's value for
   the setting that replaced it, as --key=value is, unless the command line or a variable before it gives that key, and
   the log names it with the value in effect */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv */
#endif
#include "nx/runtime/ini.h"
#include "nx/runtime/options.h"
#include "snx_settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: %s\nlog:\n%s", __LINE__, what, g_log);                                               \
        }                                                                                                              \
    } while (0)

/* options.c's overrides, which deprecated_env.c gives its values to */
static struct {
    const char* key;
    const char* value;
} g_over[16];
static int g_nover;
void ini_override(const char* k, const char* v) {
    int i;
    for (i = 0; i < g_nover && strcmp(g_over[i].key, k); i++) continue;
    if (i == g_nover) g_over[g_nover++].key = k;
    g_over[i].value = v;
}
const char* ini_overridden(const char* k) {
    for (int i = 0; i < g_nover; i++)
        if (!strcmp(g_over[i].key, k)) return g_over[i].value;
    return NULL;
}
static char g_log[4096];
void logf_(int level, const char* fmt, ...) {
    size_t n = strlen(g_log);
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
    va_end(ap);
}

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : ""); /* an empty value removes it */
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
static const char* const g_names[] = {"SNX_NO_WW", "SNX_WW", "SNX_NO_TRAPS", "SNX_TRAPS", "SNX_HOST_PAGE"};
/* nothing set, every setting at its default, no override */
static void start(void) {
    for (size_t i = 0; i < sizeof g_names / sizeof *g_names; i++) set_env(g_names[i], NULL);
    g_nover = 0;
    g_log[0] = 0;
    g_settings.write_tracking = g_settings.memory_traps = g_settings.host_page_size = 0;
}
/* what ini_open does: the variables, the overrides applied as the file's settings, then the log's lines */
static void run(void) {
    options_old_environment();
    for (int i = 0; i < g_nover; i++) ini_read(ini_find(g_debug_ini, g_over[i].key), g_over[i].value);
    options_old_environment_say();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    start();
    run();
    CHECK(!g_nover && !g_log[0], "with no variable set, something was given or said");

    start();
    set_env("SNX_WW", "signal");
    run();
    CHECK(g_settings.write_tracking == 1, "SNX_WW=signal is not write_tracking=1");
    CHECK(!strcmp(g_log, "settings: SNX_WW=signal is deprecated: this run reads it as write_tracking=1, a setting of "
                         "the game's .ini and of --write_tracking=VALUE (docs/DEPRECATIONS.md)\n"),
          "SNX_WW=signal is not said with write_tracking=1");

    start();
    set_env("SNX_HOST_PAGE", "65536");
    run();
    CHECK(g_settings.host_page_size == 65536, "SNX_HOST_PAGE=65536 is not host_page=65536");
    CHECK(strstr(g_log, "settings: SNX_HOST_PAGE=65536 is deprecated: this run reads it as host_page=65536,"),
          "SNX_HOST_PAGE=65536 is not said with host_page=65536");

    /* off came first, as the runtime read them: SNX_NO_WW over SNX_WW=signal */
    start();
    set_env("SNX_NO_WW", "1");
    set_env("SNX_WW", "signal");
    run();
    CHECK(g_settings.write_tracking == 2, "SNX_NO_WW with SNX_WW=signal is not write_tracking=2");
    CHECK(strstr(g_log, "settings: SNX_NO_WW=1 is deprecated: this run reads it as write_tracking=2,"),
          "SNX_NO_WW=1 is not said with write_tracking=2");
    CHECK(strstr(g_log, "settings: SNX_WW=signal is deprecated, and SNX_NO_WW gives write_tracking first: "
                        "write_tracking=2 this run (docs/DEPRECATIONS.md)\n"),
          "SNX_WW=signal under SNX_NO_WW is not said with write_tracking=2");

    /* the command line's own --key comes first */
    start();
    ini_override("memory_traps", "2");
    set_env("SNX_TRAPS", "signal");
    run();
    CHECK(g_settings.memory_traps == 2, "SNX_TRAPS=signal took memory_traps from the command line's 2");
    CHECK(!strcmp(g_log, "settings: SNX_TRAPS=signal is deprecated, and the command line gives memory_traps first: "
                         "memory_traps=2 this run (docs/DEPRECATIONS.md)\n"),
          "SNX_TRAPS=signal under the command line is not said with memory_traps=2");

    /* a value the runtime never read: the setting is the file's */
    start();
    g_settings.write_tracking = 2; /* as the game's .ini sets it */
    set_env("SNX_WW", "yes");
    options_old_environment();
    options_old_environment_say();
    CHECK(!g_nover, "SNX_WW=yes gave write_tracking a value");
    CHECK(!strcmp(g_log, "settings: SNX_WW=yes is deprecated, and only SNX_WW=signal did anything: write_tracking=2 "
                         "this run (docs/DEPRECATIONS.md)\n"),
          "SNX_WW=yes is not said with the file's write_tracking=2");

    start();
    printf("deprecated_env: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
