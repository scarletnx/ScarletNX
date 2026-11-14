/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The route list. hookgen --routes over tests/hook/routes.hook, versions.hook and address.hook: every symbol a hook or
   a route line names and every address hook's key, once, sorted by strcmp, however many versions declare it and
   however many specs name it. Then the runtime's own list (src/nx/loader/hook_routes.c, from hooks.hook, nvn.hook and
   the game patches): its lookup, which the loader keeps a module's exported functions by, and the hashes a module
   carries in snx_routes for hook_install. Then an address hook's key against the build IDs it is for */
#include "nx/loader/hook_routes.h"
#include "common/utilities/bits.h"
#include "nx/loader/hook_routes.inc"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_generated(void) {
    static const char* const expected[] = {"0123456789abcdef:1000",
                                           "0123456789abcdef:40",
                                           "_ZN2nn2os4WaitEv",
                                           "any",
                                           "exact",
                                           "gap",
                                           "lock",
                                           "nvn_only",
                                           "unlock",
                                           "vec_*_dtor",
                                           "vec_i*",
                                           "vec_int_dtor",
                                           "wrapped"};
    assert(HOOK_ROUTE_COUNT == sizeof expected / sizeof *expected);
    for (size_t i = 0; i < HOOK_ROUTE_COUNT; ++i) assert(!strcmp(hook_route_names[i], expected[i]));
    assert(hook_route_names[HOOK_ROUTE_COUNT] == NULL);
    assert(HOOK_ROUTE_PATTERNS == 2 && hook_route_patterns[0] == 9 && hook_route_patterns[1] == 10);
    puts("PASS hookgen --routes: each symbol of three specs once, sorted, with the route lines and address keys");
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
    assert(!snx_hook_routed("strxfrm") && !snx_hook_routed("") && !snx_hook_routed("memcpy2"));
    /* a pattern routes every symbol it matches */
    assert(snx_hook_routed("_ZNSt3__113__vector_baseImNS_9allocatorImEEED2Ev"));
    assert(snx_hook_routed("_ZNSt3__113__vector_baseI8SLOTLOOTNS_9allocatorIS1_EEED2Ev"));
    assert(!snx_hook_routed("_ZNSt3__113__vector_baseI8SLOTLOOTNS_9allocatorIS1_EEED0Ev"));
    assert(snx_hook_route_hash("memcpy") == snx_fnv1a(SNX_FNV_BASIS, "memcpy", 6));
    printf("PASS the runtime's route list: %zu symbols, sorted, each found, list hash %016llx\n", count,
           (unsigned long long)list);
}

/* a key names its build by the build ID's first bytes, the rest of the ID zeros: never a symbol, another build or a
   longer ID that only starts the same */
static void test_keys(void) {
    unsigned char id[32] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}, other[32];
    SnxU64 offset = 0;
    assert(snx_hook_key_at("0123456789abcdef:40", id, &offset) && offset == 0x40);
    assert(snx_hook_key_at("0123456789ABCDEF:d02de0", id, &offset) && offset == 0xd02de0);
    memcpy(other, id, sizeof id);
    other[8] = 1; /* an ID that goes on past the key's bytes is another build's */
    assert(!snx_hook_key_at("0123456789abcdef:40", other, &offset));
    assert(snx_hook_key_at("0123456789abcdef01:44", other, &offset) && offset == 0x44);
    other[0] = 0x02;
    assert(!snx_hook_key_at("0123456789abcdef01:44", other, &offset));
    assert(!snx_hook_key_at("memcpy", id, &offset) && !snx_hook_key_at("", id, &offset));
    assert(!snx_hook_key_at("0123456789abcdef", id, &offset) && !snx_hook_key_at("0123456789abcdef:", id, &offset));
    assert(!snx_hook_key_at("0123456789abcde:40", id, &offset) && !snx_hook_key_at("0123456789abcdef:4x", id, &offset));
    assert(!snx_hook_key_at("0123456789abcdef:40 ", id, &offset));
    puts("PASS address keys: the build each names, and the names that are none");
}

int main(void) {
    test_generated();
    test_runtime_list();
    test_keys();
    return 0;
}
