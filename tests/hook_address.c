/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Address hooks and a table of two specs (tests/hook/address_first.hook, then address.hook), as hooks.hook is read
   with the game patches of src/nx/patches/: each spec's families in their order, an address hook's key for its symbol
   (the build ID in lower case without its ending zero bytes, the offset without 0x or leading zeros), sorted with the
   symbols, and its adapter, which calls the handler its name gives */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdio.h>

static u64 seen;
static f32 seen_f;
static int calls;

u32 test_plain(CPU* c, u32 a) {
    (void)c;
    return a + 1;
}
u64 test_at(CPU* c, u64 a, f32 b) {
    (void)c;
    seen = a, seen_f = b;
    return a * 2;
}
void test_at_other(CPU* c) {
    (void)c;
    calls++;
}

static int entry_of(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return i;
    assert(!"no such hook");
    return -1;
}

int main(void) {
    static CPU c;
    u32 bits;
    int at, other, plain;
    assert(HOOK_COUNT == 3 && HOOK_FAMILIES == 2);
    assert(!strcmp(hook_family_name[0], "plain") && !strcmp(hook_family_name[1], "patch"));
    /* the keys sort before the symbols, as digits come before letters */
    assert(!strcmp(hook_symbol[0], "0123456789abcdef:1000") && !strcmp(hook_symbol[1], "0123456789abcdef:40"));
    assert(!strcmp(hook_symbol[2], "plain"));
    at = entry_of("0123456789abcdef:40"), other = entry_of("0123456789abcdef:1000"), plain = entry_of("plain");
    assert(hook_family[plain] == 0 && hook_family[at] == 1 && hook_family[other] == 1);
    assert(!strcmp(hook_handler[at], "test_at") && !strcmp(hook_handler[other], "test_at_other"));
    assert(hook_key[at] == HOOK_ANY && hook_lo[at] == 0 && hook_hi[at] == 0xFFFFFFFFu);

    memset(&c, 0, sizeof c);
    c.x[0] = 21, c.x[30] = 0x9000;
    bits = 0x40400000u; /* 3.0f */
    SNX_VSET_S(c.v[0], 0, bits);
    hook_fn[at](&c, 0x1040);
    assert(seen == 21 && seen_f == 3.0f && c.x[0] == 42 && c.pc == 0x9000);
    c.x[30] = 0x9100;
    hook_fn[other](&c, 0x2000);
    assert(calls == 1 && c.pc == 0x9100);
    c.x[0] = 7, c.x[30] = 0x9200;
    hook_fn[plain](&c, 0x3000);
    assert(c.x[0] == 8 && c.pc == 0x9200);
    puts("PASS address hooks: two specs in one table, the keys normalized and sorted, their adapters");
    return 0;
}
