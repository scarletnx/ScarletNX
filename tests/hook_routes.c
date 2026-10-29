/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The route list. hookgen --routes over tests/hook/routes.hook and versions.hook: every symbol a hook or a route line
   names, once, sorted by strcmp, however many versions declare it and however many specs name it. Then the runtime's
   own list (src/nx/loader/hook_routes.c, from hooks.hook and nvn.hook): its lookup, which the loader keeps a module's
   exported functions by, and the hashes a module carries in snx_routes for hook_install */
#include "nx/loader/hook_routes.h"
#include "common/utilities/bits.h"
#include "nx/loader/hook_routes.inc"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_generated(void) {
    static const char* const expected[] = {"_ZN2nn2os4WaitEv", "any",    "exact",  "gap", "lock",
                                           "nvn_only",         "unlock", "wrapped"};
    assert(HOOK_ROUTE_COUNT == sizeof expected / sizeof *expected);
    for (size_t i = 0; i < HOOK_ROUTE_COUNT; ++i) assert(!strcmp(hook_route_names[i], expected[i]));
    assert(hook_route_names[HOOK_ROUTE_COUNT] == NULL);
    puts("PASS hookgen --routes: each symbol of two specs once, sorted, with the route lines");
}

static void test_runtime_list(void) {
    size_t count = snx_hook_route_count();
    SnxU64 list = SNX_FNV_BASIS;
    assert(count > 3);
    for (size_t i = 0; i < count; ++i) {
        const char* name = snx_hook_route_name(i);
        assert(name && snx_hook_routed(name));
        assert(!i || strcmp(snx_hook_route_name(i - 1), name) < 0);
        list = snx_fnv_step(list, snx_hook_route_hash(name));
    }
    assert(snx_hook_route_name(count) == NULL);
    assert(list == snx_hook_route_list_hash());
    /* hooks.hook's libc hooks, nvn.hook's functions and its route lines; not a function no hook names */
    assert(snx_hook_routed("memcpy") && snx_hook_routed("memset") && snx_hook_routed("nvnQueueSubmitCommands"));
    assert(snx_hook_routed("nvnBootstrapLoader") && snx_hook_routed("nvnDeviceGetProcAddress"));
    assert(!snx_hook_routed("strlen") && !snx_hook_routed("") && !snx_hook_routed("memcpy2"));
    assert(snx_hook_route_hash("memcpy") == snx_fnv1a(SNX_FNV_BASIS, "memcpy", 6));
    printf("PASS the runtime's route list: %zu symbols, sorted, each found, list hash %016llx\n", count,
           (unsigned long long)list);
}

int main(void) {
    test_generated();
    test_runtime_list();
    return 0;
}
