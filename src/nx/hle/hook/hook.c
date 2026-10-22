/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks (hook.h): the generated table of hooks.hook, matched against the game's exported functions by name, each
   match's dispatch slot given the hook's adapter */
#include "nx/hle/hook/hook.h"
#include "nx/hle/hle.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"

/* the hook named symbol, -1 for none: a binary search of the table hookgen sorted */
static int hook_find(const char* symbol) {
    int lo = 0, hi = HOOK_COUNT;
    while (lo < hi) {
        int middle = lo + (hi - lo) / 2, order = strcmp(hook_symbol[middle], symbol);
        if (!order) return middle;
        if (order < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return -1;
}

/* whether the hooks_off setting names family: one of its words, separated by commas or spaces */
static int family_off(const char* family) {
    const char* p = g_settings.hook_families_off;
    size_t n = strlen(family);
    while (*p) {
        size_t k;
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        for (k = 0; p[k] && p[k] != ',' && p[k] != ' ' && p[k] != '\t'; k++) {
        }
        if (k == n && !memcmp(p, family, n)) return 1;
        p += k;
    }
    return 0;
}

static int hooked_at(u64 addr) {
    const HleHook* h = &g_hle.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (h->addr[i] == addr) return 1;
    return 0;
}

void hook_install(void) {
    HleHook* h = &g_hle.hook;
    const char* name;
    u64 addr;
    u32 i;
    unsigned char off[HOOK_FAMILIES + 1] = {0};
    if (!g_settings.native_hooks) {
        LOG("hooks: off (hooks=0), every exported function runs its own code\n");
        return;
    }
    if (g_arm_mod || !g_disp) return; /* a 32-bit game's code is arm.c's blocks, which no hook replaces */
    for (i = 0; i < HOOK_FAMILIES; i++)
        if ((off[i] = (unsigned char)family_off(hook_family_name[i])) != 0)
            LOG("hooks: the %s family is off (hooks_off)\n", hook_family_name[i]);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        int k = name ? hook_find(name) : -1;
        GuestFn* slot;
        if (k < 0 || off[hook_family[k]] || addr < g_code_lo || addr >= g_code_hi || (addr & 3) || hooked_at(addr))
            continue;
        if (h->count == HOOK_SITES) {
            WARN("hooks: more than %d functions to hook, so %s at %llx and those after it run their own code\n",
                 HOOK_SITES, name, (unsigned long long)addr);
            break;
        }
        slot = &g_disp[(addr - g_code_lo) >> 2];
        h->hook[h->count] = (u16)k;
        h->addr[h->count] = addr;
        h->orig[h->count] = *slot;
        h->count++;
        __atomic_store_n(slot, hook_fn[k], __ATOMIC_RELEASE);
        LOG("hook: %s at %llx runs %s\n", name, (unsigned long long)addr, hook_handler[k]);
    }
    LOG("hooks: %u of the game's functions run the runtime's handlers\n", h->count);
}

void hook_restore(u64 addr, u64 size) {
    const HleHook* h = &g_hle.hook;
    MemBlock block;
    u32 i;
    for (i = 0; i < h->count; i++) {
        u64 a = h->addr[i];
        if (a < addr || a - addr >= size) continue;
        mem_query(a, &block);
        if (block.state == MS_FREE || !(block.perm & PERM_X)) continue; /* no longer code: the hook goes with it */
        __atomic_store_n(&g_disp[(a - g_code_lo) >> 2], hook_fn[h->hook[i]], __ATOMIC_RELEASE);
    }
}

GuestFn hook_original(u64 addr) {
    const HleHook* h = &g_hle.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (h->addr[i] == addr) return h->orig[i];
    return NULL;
}
