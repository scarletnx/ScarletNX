/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks (hook.h): the generated table of hooks.hook, matched against the game's exported functions by name and the
   version of the module that exports each, each match's dispatch slot given the adapter of the entry for that
   version */
#include "nx/hle/hook/hook.h"
#include "nx/hle/hle.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"

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

/* the modules whose versions hook_install reads; a module past them has none, so it gets only unversioned hooks */
#define HOOK_MODULES 32

/* the module whose mapping holds addr, -1 for none */
static int module_of(u64 addr) {
    u32 i;
    for (i = 0; i < g_mod->nmods; i++)
        if (addr >= g_mod->mods[i].base && addr - g_mod->mods[i].base < g_mod->mods[i].size) return (int)i;
    return -1;
}

/* each module's nnSdk version, from the SDK's label in its read-only data, logged */
static void module_versions(u32 sdk[HOOK_MODULES]) {
    u32 i;
    for (i = 0; i < g_mod->nmods && i < HOOK_MODULES; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        char text[32];
        sdk[i] = 0;
        if (mi->ro_size && mi->ro_off <= mi->size && mi->ro_size <= mi->size - mi->ro_off)
            sdk[i] = hook_label_version(GP(mi->base + mi->ro_off), mi->ro_size, "nnSdk");
        hook_version_text(text, sizeof text, HOOK_SDK, sdk[i]);
        LOG("hooks: module %s, SDK %s%s\n", mi->name, text, sdk[i] ? "" : " (its versioned hooks aren't installed)");
    }
    if (g_mod->nmods > HOOK_MODULES)
        WARN("hooks: %u modules, so the SDK versions of those past %d are unknown\n", g_mod->nmods, HOOK_MODULES);
}

void hook_install(void) {
    HleHook* h = &g_hle.hook;
    const char* name;
    u64 addr;
    u32 i, sdk[HOOK_MODULES] = {0};
    unsigned char off[HOOK_FAMILIES + 1] = {0};
    unsigned char said[HOOK_COUNT + 1] = {0};   /* the entries whose versions the log gave */
    unsigned char missed[HOOK_COUNT + 1] = {0}; /* the symbols (by their first entry) the log said had none */
    if (!g_settings.native_hooks) {
        LOG("hooks: off (hooks=0), every exported function runs its own code\n");
        return;
    }
    if (g_arm_mod || !g_disp || !g_mod) return; /* a 32-bit game's code is arm.c's blocks, which no hook replaces */
    for (i = 0; i < HOOK_FAMILIES; i++)
        if ((off[i] = (unsigned char)family_off(hook_family_name[i])) != 0)
            LOG("hooks: the %s family is off (hooks_off)\n", hook_family_name[i]);
    module_versions(sdk);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        u32 versions[HOOK_KEYS] = {0};
        int m, first, k;
        char range[96];
        GuestFn* slot;
        if (!name || addr < g_code_lo || addr >= g_code_hi || (addr & 3) ||
            hook_select(&hook_table, name, versions, &first) == -1 || (m = module_of(addr)) < 0 || hooked_at(addr))
            continue;
        /* the versions of the module that exports it; the NVN API version isn't read yet, so it is unknown */
        versions[HOOK_SDK] = m < HOOK_MODULES ? sdk[m] : 0;
        k = hook_select(&hook_table, name, versions, &first);
        if (k == -2 && !missed[first]) {
            char text[32];
            missed[first] = 1;
            hook_version_text(text, sizeof text, hook_key[first], versions[hook_key[first]]);
            LOG("hooks: %s in %s isn't hooked: no declaration holds for its version (%s)\n", name, g_mod->mods[m].name,
                text);
        }
        if (k < 0 || off[hook_family[k]]) continue;
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
        if (hook_key[k] == HOOK_ANY || said[k]) {
            LOG("hook: %s at %llx runs %s\n", name, (unsigned long long)addr, hook_handler[k]);
            continue;
        }
        said[k] = 1; /* a versioned hook's versions and layouts, the first time it is installed */
        hook_range_text(range, sizeof range, hook_key[k], hook_lo[k], hook_hi[k]);
        LOG("hook: %s at %llx runs %s, the hook for %s%s%s\n", name, (unsigned long long)addr, hook_handler[k], range,
            *hook_layouts[k] ? ", with the layouts " : "", hook_layouts[k]);
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
