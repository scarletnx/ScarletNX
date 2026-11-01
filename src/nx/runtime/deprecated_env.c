/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The environment variables an older version read for what are settings of the game's .ini now (docs/DEPRECATIONS.md).
   For one version each one set is still this run's value for its setting, as --key=value is, unless the command line
   or a variable before it gives that key; once the settings are read, the log names it with the value in effect. The
   runtime's only getenv of a setting (tests/architecture.py's ENV_DEPRECATED), removed with those rows */
#include "nx/runtime/ini.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OLD_COPY,   /* its value is the setting's */
    OLD_SET,    /* set at all, to anything: the setting is `to` */
    OLD_SIGNAL, /* "signal" alone: the setting is `to`; another value did nothing */
    OLD_SWITCH, /* empty or starting with 0: the setting is 0, else 1 */
};
typedef struct {
    const char* name;  /* the variable */
    const char* value; /* its value; NULL when it is not set */
    int how;
    const char* key; /* the setting that replaced it */
    const char* to;  /* OLD_SET's and OLD_SIGNAL's value of the setting */
} OldVariable;
#define OLD_MAX 8
static struct {
    OldVariable set[OLD_MAX];   /* the variables set this run, in the table's order */
    const char* given[OLD_MAX]; /* the value each gave its setting, NULL for none */
    const char* first[OLD_MAX]; /* what gave that setting before it: "the command line", a variable's name, or NULL */
    int count;
} g_deprecated_env;

void options_old_environment(void) {
    /* SNX_NO_WW before SNX_WW, SNX_NO_TRAPS before SNX_TRAPS: off came first, as the runtime read them */
    const OldVariable table[] = {
        {"SNX_NO_WW", getenv("SNX_NO_WW"), OLD_SET, "write_tracking", "2"},
        {"SNX_WW", getenv("SNX_WW"), OLD_SIGNAL, "write_tracking", "1"},
        {"SNX_NO_TRAPS", getenv("SNX_NO_TRAPS"), OLD_SET, "memory_traps", "2"},
        {"SNX_TRAPS", getenv("SNX_TRAPS"), OLD_SIGNAL, "memory_traps", "1"},
        {"SNX_HOST_PAGE", getenv("SNX_HOST_PAGE"), OLD_COPY, "host_page", NULL},
        {"SNX_ALIAS_CHECK", getenv("SNX_ALIAS_CHECK"), OLD_SWITCH, "gpu_page_check", NULL},
        {"SNX_DIRTY_CHECK", getenv("SNX_DIRTY_CHECK"), OLD_SWITCH, "draw_state_check", NULL},
        {"SNX_LLVM_BRANCH_PROFILE", getenv("SNX_LLVM_BRANCH_PROFILE"), OLD_COPY, "ir_branch_profile", NULL},
    };
    size_t i;
    int j, n = 0;
    (void)sizeof(char[sizeof table / sizeof *table <= OLD_MAX ? 1 : -1]); /* OLD_MAX holds every row */
    for (i = 0; i < sizeof table / sizeof *table; i++) {
        const OldVariable* v = &table[i];
        const char *given = NULL, *first = NULL;
        if (!v->value) continue;
        if (v->how == OLD_COPY)
            given = v->value;
        else if (v->how == OLD_SWITCH)
            given = v->value[0] && v->value[0] != '0' ? "1" : "0";
        else if (v->how == OLD_SET || !strcmp(v->value, "signal"))
            given = v->to;
        for (j = 0; j < n && !first; j++)
            if (g_deprecated_env.given[j] && !g_deprecated_env.first[j] && !strcmp(g_deprecated_env.set[j].key, v->key))
                first = g_deprecated_env.set[j].name;
        if (!first && ini_overridden(v->key)) first = "the command line";
        if (given && !first) ini_override(v->key, given);
        g_deprecated_env.set[n] = *v;
        g_deprecated_env.given[n] = given;
        g_deprecated_env.first[n++] = first;
    }
    g_deprecated_env.count = n;
}

void options_old_environment_say(void) {
    int i;
    for (i = 0; i < g_deprecated_env.count; i++) {
        const OldVariable* v = &g_deprecated_env.set[i];
        const IniOpt* o = ini_find(g_debug_ini, v->key);
        char now[1100];
        if (!o) continue;
        ini_format(o, now, sizeof now);
        if (g_deprecated_env.first[i])
            WARN("settings: %s=%s is deprecated, and %s gives %s first: %s=%s this run (docs/DEPRECATIONS.md)\n",
                 v->name, v->value, g_deprecated_env.first[i], v->key, v->key, now);
        else if (g_deprecated_env.given[i])
            WARN("settings: %s=%s is deprecated: this run reads it as %s=%s, a setting of the game's .ini and of "
                 "--%s=VALUE (docs/DEPRECATIONS.md)\n",
                 v->name, v->value, v->key, now, v->key);
        else
            WARN("settings: %s=%s is deprecated, and only %s=signal did anything: %s=%s this run "
                 "(docs/DEPRECATIONS.md)\n",
                 v->name, v->value, v->name, v->key, now);
    }
}
