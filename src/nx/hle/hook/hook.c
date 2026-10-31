/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks (hook.h): the generated table of hooks.hook, matched against the game's exported functions by name and the
   version of the module that exports each, each match's dispatch slot given the adapter of the entry for that
   version */
#include "nx/hle/hook/hook.h"
#include "common/utilities/bits.h"
#include "nx/hle/hle.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"

/* whether a setting's list (hooks_off, hooks_verify) names family: one of its words, separated by commas or spaces */
static int family_listed(const char* list, const char* family) {
    const char* p = list;
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

/* the site of the hooked function at addr; -1 for none */
static int site_of(u64 addr) {
    const HleHook* h = &g_hle.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (h->addr[i] == addr) return (int)i;
    return -1;
}

/* a scratch slot of verify mode's, -1 when every one is taken */
static int verify_claim(void) {
    u32* taken = &g_hle.hook.verify_taken;
    u32 seen = __atomic_load_n(taken, __ATOMIC_RELAXED);
    for (;;) {
        int i;
        if (seen == ~0u >> (32 - HOOK_VERIFY_SLOTS)) return -1;
        for (i = 0; seen & (1u << i); i++) {
        }
        if (__atomic_compare_exchange_n(taken, &seen, seen | (1u << i), 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return i;
    }
}

/* a count of verify mode's worth logging: the first, and each power of 4 from 1024 */
static int verify_milestone(u64 n) { return n == 1 || (n >= 1024 && !(n & (n - 1)) && snx_ctz64(n) % 2 == 0); }

/* verify mode's check of a pure hook (verify.h), in the function's slot in place of the hook's adapter: each call
   runs the function's own code on copies of its buffers, then the handler, and the log names the calls whose
   results, registers or bytes differ */
static void verify_fn(CPU* RESTRICT c, u64 entry) {
    HleHook* h = &g_hle.hook;
    int i = site_of(entry), slot, what;
    u64 args[8], lr = c->x[30];
    HookVerifyResult r;
    u16 k;
    if (i < 0) { /* not a site: cannot happen, as only hook_install puts this in a slot */
        fatal("hook verify: no hook at %llx", (unsigned long long)entry);
    }
    k = h->hook[i];
    memcpy(args, c->x, sizeof args);
    slot = verify_claim();
    what = slot < 0 ? HOOK_SKIP_SIZE
                    : hook_verify_call(c, entry, h->orig[i], hook_fn[k], &hook_verify[k],
                                       h->verify_base + (u64)slot * HOOK_VERIFY_SLOT, HOOK_VERIFY_SLOT, &r);
    if (slot >= 0) __atomic_and_fetch(&h->verify_taken, ~(1u << slot), __ATOMIC_RELEASE);
    if (what >= HOOK_SKIP_SIZE) { /* nothing ran: the handler does the call */
        u64 n = __atomic_add_fetch(&h->skipped[i], 1, __ATOMIC_RELAXED);
        hook_fn[k](c, entry);
        if (verify_milestone(n))
            LOG("hook verify: %s, %llu calls skipped (%s), the last of %llu bytes\n", hook_symbol[k],
                (unsigned long long)n, slot < 0 ? "no scratch slot free" : hook_verify_text(what),
                (unsigned long long)args[2]);
        return;
    }
    if (what == HOOK_AGREE) {
        u64 n = __atomic_add_fetch(&h->agreed[i], 1, __ATOMIC_RELAXED);
        if (verify_milestone(n))
            LOG("hook verify: %s at %llx, %llu calls agree with its own code (%llu differ, %llu skipped)\n",
                hook_symbol[k], (unsigned long long)entry, (unsigned long long)n,
                (unsigned long long)__atomic_load_n(&h->differed[i], __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&h->skipped[i], __ATOMIC_RELAXED));
        return;
    }
    if (__atomic_add_fetch(&h->differed[i], 1, __ATOMIC_RELAXED) <= 16)
        WARN("hook verify: %s at %llx, called from %llx with %llx %llx %llx %llx, %s: the handler's %llx, its own "
             "code's %llx (register %d, buffer %d at +%llx)\n",
             hook_symbol[k], (unsigned long long)entry, (unsigned long long)lr, (unsigned long long)args[0],
             (unsigned long long)args[1], (unsigned long long)args[2], (unsigned long long)args[3],
             hook_verify_text(what), (unsigned long long)r.a, (unsigned long long)r.b, r.reg, r.buf,
             (unsigned long long)r.offset);
}

int hook_routed(const char* name) {
    const SnxRoutes* r = g_routes;
    if (!r || r->all) return 1;
    u64 hash = snx_hook_route_hash(name);
    size_t lo = 0, hi = r->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (r->names[middle] < hash)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < r->count && r->names[lo] == hash;
}

/* which calls the module sends through the dispatch slots, once */
static void routes_say(void) {
    const SnxRoutes* r = g_routes;
    u64 own = snx_hook_route_list_hash();
    if (!r)
        LOG("hooks: the module calls every function it exports through its dispatch slot (it has no route list)\n");
    else if (r->all)
        LOG("hooks: the module calls every function through its dispatch slot (--hook-direct-calls)\n");
    else if (r->list == own)
        LOG("hooks: the module calls the functions of the %u symbols the hooks name through their dispatch slots, and "
            "every other function directly (route list %016llx, this runtime's too)\n",
            (unsigned)r->count, (unsigned long long)r->list);
    else
        LOG("hooks: the module calls the functions of the %u symbols its build's hooks named through their dispatch "
            "slots, and every other function directly (route list %016llx; this runtime's, %016llx, names %u)\n",
            (unsigned)r->count, (unsigned long long)r->list, (unsigned long long)own, (unsigned)snx_hook_route_count());
}

void hook_install(void) {
    HleHook* h = &g_hle.hook;
    const char* name;
    u64 addr;
    u32 i, sdk[HOOK_MODULES] = {0};
    unsigned char off[HOOK_FAMILIES + 1] = {0}, verify[HOOK_FAMILIES + 1] = {0};
    int any_verify = 0;
    unsigned char said[HOOK_COUNT + 1] = {0};   /* the entries whose versions the log gave */
    unsigned char missed[HOOK_COUNT + 1] = {0}; /* the symbols (by their first entry) the log said had none */
    if (!g_settings.native_hooks) {
        LOG("hooks: off (hooks=0), every exported function runs its own code\n");
        return;
    }
    if (g_arm_mod || !g_disp || !g_mod) return; /* a 32-bit game's code is arm.c's blocks, which no hook replaces */
    routes_say();
    for (i = 0; i < HOOK_FAMILIES; i++)
        if ((off[i] = (unsigned char)family_listed(g_settings.hook_families_off, hook_family_name[i])) != 0)
            LOG("hooks: the %s family is off (hooks_off)\n", hook_family_name[i]);
        else if (family_listed(g_settings.hook_families_verified, hook_family_name[i]))
            verify[i] = 1, any_verify = 1;
    if (any_verify) {
        h->verify_base = kmem_alloc((u64)HOOK_VERIFY_SLOTS * HOOK_VERIFY_SLOT, MS_KERNEL, PERM_RW);
        LOG("hooks: verify mode for the pure hooks of the families hooks_verify names, with %u KiB of scratch at %llx\n",
            (HOOK_VERIFY_SLOTS * HOOK_VERIFY_SLOT) >> 10, (unsigned long long)h->verify_base);
    }
    module_versions(sdk);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        u32 versions[HOOK_KEYS] = {0};
        int m, first, k;
        char range[96];
        GuestFn* slot;
        if (!name || addr < g_code_lo || addr >= g_code_hi || (addr & 3) ||
            hook_select(&hook_table, name, versions, &first) == -1 || (m = module_of(addr)) < 0 || site_of(addr) >= 0)
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
        h->fn[h->count] = verify[hook_family[k]] && hook_verify[k].pure ? verify_fn : hook_fn[k];
        __atomic_store_n(slot, h->fn[h->count], __ATOMIC_RELEASE);
        h->count++;
        if (!hook_routed(name))
            WARN("hooks: %s at %llx runs %s only for the calls through the PLT and function pointers: the module's "
                 "route list doesn't name it, so its direct calls run its own code. Recompile the game for them\n",
                 name, (unsigned long long)addr, hook_handler[k]);
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
        __atomic_store_n(&g_disp[(a - g_code_lo) >> 2], h->fn[i], __ATOMIC_RELEASE);
    }
}

GuestFn hook_original(u64 addr) {
    int i = site_of(addr);
    return i < 0 ? NULL : g_hle.hook.orig[i];
}
