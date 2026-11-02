/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Inserted into the program by the jemalloc test on macOS (tests/jemalloc.cmake, DYLD_INSERT_LIBRARIES): at exit, after
   the program's constructors, it prints the name of the zone a new allocation comes from, jemalloc_zone once jemalloc's
   is the default one. jemalloc starts at load whether or not its zone went in front, so its statistics do not say */
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>

/* a global the compiler cannot see through, or it would take the malloc and free out */
static void* volatile g_jemalloc_zone_kept;

__attribute__((destructor)) static void jemalloc_zone_print(void) {
    const malloc_zone_t* zone;
    g_jemalloc_zone_kept = malloc(64);
    zone = malloc_zone_from_ptr(g_jemalloc_zone_kept);
    fprintf(stderr, "snx-jemalloc-zone: %s\n", zone && zone->zone_name ? zone->zone_name : "(none)");
    free(g_jemalloc_zone_kept);
}
