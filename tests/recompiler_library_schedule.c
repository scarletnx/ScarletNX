/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The order the library's code units are compiled in, white box (library.c's chunks and compare_unit_weight): the
   heaviest first, by instructions plus four per leader, so the unit that finishes last is a small one. The three
   functions of recompiler_library.c's schedule64: entry (3 instructions), wide (4501 in one block) and branchy (2001 in
   1001 blocks). With 4 jobs the units are {entry, wide} and {branchy}, and branchy's, the heavier though it has fewer
   instructions and comes later in the code, is taken first. And the module cache's name: the native back end's code
   version (snx_native_code) is part of it */
#include "common/cpu/recompiler/library.c"

static int t_failures;
static void t_check(int ok, const char* what) {
    if (ok) return;
    printf("FAIL: %s\n", what);
    ++t_failures;
}

int main(void) {
    SnxFunction f[3] = {{0}};
    f[0].start = 0x100000, f[0].count = 3, f[0].leader_count = 1;
    f[1].start = 0x10000c, f[1].count = 4501, f[1].leader_count = 1;
    f[2].start = 0x104620, f[2].count = 2001, f[2].leader_count = 1001;
    SnxCodegen codegen = {0};
    codegen.functions = f;
    codegen.function_count = 3;
    size_t count = 0;
    size_t* units = chunks(&codegen, 4, &count);
    char what[160];
    snprintf(what, sizeof what, "4 jobs: the units {entry, wide} and {branchy} (%zu units, from %zu and %zu)", count,
             units ? units[0] : 0, units && count > 1 ? units[1] : 0);
    t_check(units && count == 2 && units[0] == 0 && units[1] == 2 && units[2] == 3, what);
    if (units && count == 2) {
        Unit u[2] = {{.first = units[0], .last = units[1]}, {.first = units[1], .last = units[2]}};
        for (size_t i = 0; i < 2; ++i)
            for (size_t j = u[i].first; j < u[i].last; ++j) u[i].weight += function_weight(&f[j]);
        qsort(u, 2, sizeof *u, compare_unit_weight);
        snprintf(what, sizeof what, "branchy's unit first (weight %zu), then entry and wide's (%zu)", u[0].weight,
                 u[1].weight);
        t_check(u[0].first == 2 && u[1].first == 0 && u[0].weight > u[1].weight, what);
    }
    free(units);
    /* a host without a native back end has no recompiler, and so no cache */
    struct snx_recompiler* r = snx_native_available() ? snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM) : NULL;
    if (snx_native_available())
        t_check(r && !snx_recompiler_set_cache(r, "library-schedule-cache"), "a recompiler with a cache folder");
    if (r && r->cache) {
        SnxCodegen empty = {0}; /* the key of a program's words reads them from its image: here none */
        char* a = cache_path_of(r, &empty, "0123456789abcdef");
        char* b = cache_path_of(r, &empty, "0123456789abcdee");
        char* c = cache_path_of(r, &empty, "0123456789abcdef");
        char* now = cache_path(r, &empty);
        char* same = cache_path_of(r, &empty, snx_native_code());
        snprintf(what, sizeof what, "the cache's name follows the native code's version (%s, %s)", a ? a : "-",
                 b ? b : "-");
        t_check(a && b && c && strcmp(a, b) && !strcmp(a, c), what);
        t_check(now && same && !strcmp(now, same), "the cache's name is of this build's native code");
        free(a), free(b), free(c), free(now), free(same);
        snx_remove_tree(r->cache);
    }
    snx_recompiler_free(r);
    if (!t_failures)
        printf("the library's code units heaviest first, its cache named by the native code: as it should be\n");
    return t_failures != 0;
}
