/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Inserted into a program by the jemalloc test on macOS (tests/jemalloc.cmake, DYLD_INSERT_LIBRARIES): at exit, after
   the program's constructors, it prints the name of the zone a new allocation comes from, jemalloc_zone once jemalloc's
   is the default one, then every registered zone, the default first. jemalloc starts at load whether or not its zone
   went in front, so its statistics do not say. The zone is the first registered one whose size() claims the pointer,
   the lookup libmalloc makes: malloc_zone_from_ptr answers with the virtual default zone, named DefaultMallocZone, for
   a pointer of whichever zone is first (libmalloc's tests/malloc_zone_unregister_test.c expects it) */
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>

/* a global the compiler cannot see through, or it would take the malloc and free out */
static void* volatile g_jemalloc_zone_kept;

static const char* jemalloc_zone_name(const malloc_zone_t* zone) {
    return zone->zone_name ? zone->zone_name : "(unnamed)";
}

__attribute__((destructor)) static void jemalloc_zone_print(void) {
    vm_address_t* zones = NULL;
    unsigned count = 0;
    const char* owner = "(none)";
    kern_return_t kr;
    g_jemalloc_zone_kept = malloc(64);
    kr = malloc_get_all_zones(mach_task_self(), NULL, &zones, &count);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "snx-jemalloc-zone-error: malloc_get_all_zones returned %d\n", (int)kr);
        free(g_jemalloc_zone_kept);
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        malloc_zone_t* zone = (malloc_zone_t*)zones[i];
        if (zone->size(zone, g_jemalloc_zone_kept) != 0) {
            owner = jemalloc_zone_name(zone);
            break;
        }
    }
    fprintf(stderr, "snx-jemalloc-zone: %s\n", owner);
    fprintf(stderr, "snx-jemalloc-zones:");
    for (unsigned i = 0; i < count; i++) {
        fprintf(stderr, "%s %s", i ? "," : "", jemalloc_zone_name((const malloc_zone_t*)zones[i]));
    }
    fprintf(stderr, "\n");
    free(g_jemalloc_zone_kept);
}
