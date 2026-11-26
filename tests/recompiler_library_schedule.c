/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The order the library's code units are compiled in, white box (library.c's chunks and compare_unit_weight): the
   heaviest first, by instructions plus four per leader, so the unit that finishes last is a small one. The three
   functions of recompiler_library.c's schedule64: entry (3 instructions), wide (4501 in one block) and branchy (2001 in
   1001 blocks). With 4 jobs the units are {entry, wide} and {branchy}, and branchy's, the heavier though it has fewer
   instructions and comes later in the code, is taken first */
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
    if (!t_failures) printf("the library's code units: heaviest first, as they should be\n");
    return t_failures != 0;
}
