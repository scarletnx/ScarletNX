/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/state_fns.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/services/network/network.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/services.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

/* ipc.c's, which ipc.h declares */
static const StateFn IPC_FNS[] = {STATE_FN("ipc", stub_ok)};
static const StateFnTable g_ipc_state_fns = {IPC_FNS, 1};

/* every table: a function's name is its table's entry, and two entries never share one */
static const StateFnTable* const TABLES[] = {
    &g_swipc_fns,      &g_ipc_state_fns,  &g_am_state_fns,  &g_audio_state_fns, &g_audren_state_fns,
    &g_fs_state_fns,   &g_ldr_state_fns,  &g_misc_state_fns, &g_nvdrv_state_fns, &g_vi_state_fns,
    &g_bsd_state_fns,  &g_ldn_state_fns,  &g_nifm_state_fns, &g_ssl_state_fns,
};
enum { NTABLES = sizeof TABLES / sizeof *TABLES };

/* every entry, by name for a load and by address for a save, sorted once */
static const StateFn** g_by_name;
static const StateFn** g_by_fn;
static u32 g_n;
static int by_name(const void* a, const void* b) {
    return strcmp((*(const StateFn* const*)a)->name, (*(const StateFn* const*)b)->name);
}
static int by_fn(const void* a, const void* b) {
    uintptr_t x = (uintptr_t)(*(const StateFn* const*)a)->fn, y = (uintptr_t)(*(const StateFn* const*)b)->fn;
    return x < y ? -1 : x > y;
}
static int sorted(void) {
    u32 t, i, n = 0;
    if (g_by_name) return 1;
    for (t = 0; t < NTABLES; t++) n += TABLES[t]->n;
    g_by_name = (const StateFn**)malloc(sizeof *g_by_name * (n ? n : 1));
    g_by_fn = (const StateFn**)malloc(sizeof *g_by_fn * (n ? n : 1));
    if (!g_by_name || !g_by_fn) {
        free(g_by_name);
        free(g_by_fn);
        g_by_name = g_by_fn = NULL;
        return 0;
    }
    for (t = 0, g_n = 0; t < NTABLES; t++)
        for (i = 0; i < TABLES[t]->n; i++) {
            g_by_name[g_n] = g_by_fn[g_n] = &TABLES[t]->fn[i];
            g_n++;
        }
    qsort(g_by_name, g_n, sizeof *g_by_name, by_name);
    qsort(g_by_fn, g_n, sizeof *g_by_fn, by_fn);
    for (i = 1; i < g_n; i++)
        if (!strcmp(g_by_name[i - 1]->name, g_by_name[i]->name))
            WARN("state: two functions are named %s: a state holding either loads the first\n", g_by_name[i]->name);
    return 1;
}
const char* state_fn_name(StateFnPtr fn) {
    u32 lo = 0, hi;
    if (!fn || !sorted()) return NULL;
    hi = g_n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        uintptr_t m = (uintptr_t)g_by_fn[mid]->fn;
        if (m == (uintptr_t)fn) return g_by_fn[mid]->name;
        if (m < (uintptr_t)fn)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
StateFnPtr state_fn_find(const char* name) {
    u32 lo = 0, hi;
    if (!name || !sorted()) return NULL;
    hi = g_n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = strcmp(g_by_name[mid]->name, name);
        if (!c) return g_by_name[mid]->fn;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
