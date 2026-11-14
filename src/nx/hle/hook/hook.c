/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks (hook.h): the generated table of hooks.hook, matched against the game's exported functions by name and the
   version of the module that exports each, each match's dispatch slot given the adapter of the entry for that
   version */
#include "nx/hle/hook/hook.h"
#include "nx/hle/hook/jump.h"
#include "nx/runtime/nce/nce.h"
#include "common/utilities/bits.h"
#include "nx/hle/hle.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/linker.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "common/platform/thread.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"

/* a site for every hook of either table, so that no family is left half native (hook.h) */
typedef char hook_sites_hold_the_tables[HOOK_COUNT <= HOOK_SITES && HOOK32_COUNT <= HOOK_SITES ? 1 : -1];

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
/* the one of the game's nmods modules that addr is in; -1 for none */
static int module_of(const SnxModInfo* mods, u32 nmods, u64 addr) {
    u32 i;
    for (i = 0; i < nmods; i++)
        if (addr >= mods[i].base && addr - mods[i].base < mods[i].size) return (int)i;
    return -1;
}

/* each module's nnSdk version, from the SDK's label in its read-only data, logged */
static void module_versions(const SnxModInfo* mods, u32 nmods, u32 sdk[HOOK_MODULES]) {
    u32 i;
    for (i = 0; i < nmods && i < HOOK_MODULES; i++) {
        const SnxModInfo* mi = &mods[i];
        char text[32];
        sdk[i] = 0;
        if (mi->ro_size && mi->ro_off <= mi->size && mi->ro_size <= mi->size - mi->ro_off)
            sdk[i] = hook_label_version(GP(mi->base + mi->ro_off), mi->ro_size, "nnSdk");
        hook_version_text(text, sizeof text, HOOK_SDK, sdk[i]);
        LOG("hooks: module %s, SDK %s%s\n", mi->name, text, sdk[i] ? "" : " (its versioned hooks aren't installed)");
    }
    if (nmods > HOOK_MODULES)
        WARN("hooks: %u modules, so the SDK versions of those past %d are unknown\n", nmods, HOOK_MODULES);
}

/* a symbol with declarations, of which none holds for the version its module (the game's module'th) has of their key:
   counted, and the first kept, for hook_missed_text. first is the symbol's first entry in the game's table */
static void missed_note(int first, int module, u32 version) {
    HostHook* h = &g_host.hook;
    if (h->missed++) return;
    h->missed_entry = (u32)first;
    h->missed_module = (u32)module;
    h->missed_version = version;
}

void hook_missed_text(char* out, size_t n) {
    const HostHook* h = &g_host.hook;
    const HookTable* t = g_arm_mod ? &hook32_table : &hook_table;
    char version[32], ranges[192];
    size_t used = 0;
    u32 i;
    if (n) out[0] = 0;
    if (!h->missed) return;
    ranges[0] = 0;
    for (i = h->missed_entry; i < t->count && !strcmp(t->symbol[i], t->symbol[h->missed_entry]); i++) {
        char range[96];
        int wrote;
        hook_range_text(range, sizeof range, t->key[i], t->lo[i], t->hi[i]);
        wrote = snprintf(ranges + used, sizeof ranges - used, "%s%s", used ? "; " : "", range);
        if (wrote < 0 || (size_t)wrote >= sizeof ranges - used) break; /* cut: the log has every one */
        used += (size_t)wrote;
    }
    hook_version_text(version, sizeof version, t->key[h->missed_entry], h->missed_version);
    snprintf(out, n,
             "%u of the SDK functions the runtime has a hook for run their own code in this %s game, as none of "
             "their declarations holds for the version of the module they are in: the first is %s in %s, of "
             "version %s, declared for %s",
             (unsigned)h->missed, g_arm_mod ? "32-bit" : "64-bit", t->symbol[h->missed_entry],
             (g_arm_mod ? g_arm_mod->mods : g_mod->mods)[h->missed_module].name, version, ranges);
}

/* the site of the hooked function at addr; -1 for none */
static int site_of(u64 addr) {
    const HostHook* h = &g_host.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (h->addr[i] == addr) return (int)i;
    return -1;
}

/* the first 4 bytes of the game's code at a: one A64 or A32 instruction, or two Thumb ones */
static u32 first_word(u64 a) {
    u32 w;
    memcpy(&w, GP(a), 4);
    return w;
}

/* a scratch slot of verify mode's, -1 when every one is taken */
static int verify_claim(void) {
    u32* taken = &g_host.hook.verify_taken;
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
    HostHook* h = &g_host.hook;
    int i = site_of(entry), slot, what;
    u64 args[8], lr = c->x[30], base = g_hle.hook.verify_base;
    HookVerifyResult r;
    u16 k;
    if (i < 0) { /* not a site: cannot happen, as only hook_install puts this in a slot */
        fatal("hook verify: no hook at %llx", (unsigned long long)entry);
    }
    if (__atomic_load_n(&h->invalid[i], __ATOMIC_ACQUIRE)) { /* the handler is the game's code now */
        hook_invalidated(c, entry);
        return;
    }
    k = h->hook[i];
    memcpy(args, c->x, sizeof args);
    slot = base ? verify_claim() : -1; /* a save state loaded since holds the scratch slots only if it has their base */
    what = slot < 0 ? HOOK_SKIP_SIZE
                    : hook_verify_call(c, entry, h->orig[i], hook_fn[k], &hook_verify[k],
                                       base + (u64)slot * HOOK_VERIFY_SLOT, HOOK_VERIFY_SLOT, &r);
    if (slot >= 0) __atomic_and_fetch(&h->verify_taken, ~(1u << slot), __ATOMIC_RELEASE);
    if (what >= HOOK_SKIP_SIZE) { /* nothing ran: the handler does the call */
        u64 n = __atomic_add_fetch(&h->skipped[i], 1, __ATOMIC_RELAXED);
        hook_fn[k](c, entry);
        if (verify_milestone(n))
            LOG("hook verify: %s, %llu calls skipped (%s), the last of %llu bytes\n", hook_symbol[k],
                (unsigned long long)n,
                slot >= 0 ? hook_verify_text(what)
                : base ? "no scratch slot free"
                       : "the save state loaded has no scratch memory: hooks_verify was off in the run that saved it",
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

int hook_family_off(const char* family) {
    return !g_settings.native_hooks || family_listed(g_settings.hook_families_off, family);
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

/* the function at addr given entry k's adapter, or verify mode's check. With every site taken the game stops: its
   function would run the SDK's own code beside the hooks of its family, which keep the family's state natively */
static void hook_put(int k, u64 addr, const char* name, const unsigned char verify[]) {
    HostHook* h = &g_host.hook;
    GuestFn* slot;
    if (h->count == HOOK_SITES)
        fatal("hooks: no site left for %s at %llx: the runtime holds %d, one for each hook, and the game's modules "
              "export the functions of the hooks more than once",
              name, (unsigned long long)addr, HOOK_SITES);
    slot = &g_disp[(addr - g_code_lo) >> 2];
    h->hook[h->count] = (u16)k;
    h->addr[h->count] = addr;
    h->orig[h->count] = nce_slot(*slot);
    h->fn[h->count] = verify[hook_family[k]] && hook_verify[k].pure ? verify_fn : hook_fn[k];
    h->word[h->count] = first_word(addr);
    h->invalid[h->count] = 0;
    __atomic_store_n(slot, h->fn[h->count], __ATOMIC_RELEASE);
    nce_slot_taken(addr, hook_family_name[hook_family[k]]);
    jit_slot_taken(addr, h->fn[h->count]);
    h->count++;
    if (!hook_routed(hook_symbol[k]))
        WARN("hooks: %s at %llx runs %s only for the calls through function pointers and the PLT stubs the module "
             "did not bind: the module's route list doesn't name it, so its direct calls run its own code. Recompile "
             "the game for them\n",
             name, (unsigned long long)addr, hook_handler[k]);
}

/* the address hooks (hookgen's [BUILDID:OFFSET]), each in the module built as its build ID, at its offset there */
static void hook_put_addresses(const unsigned char off[], const unsigned char verify[]) {
    int k;
    u32 i;
    for (k = 0; k < HOOK_COUNT; k++)
        for (i = 0; i < g_mod->nmods; i++) {
            const SnxModInfo* mi = &g_mod->mods[i];
            u64 offset, addr;
            if (!snx_hook_key_at(hook_symbol[k], mi->build_id, &offset)) continue;
            addr = mi->base + offset;
            if (offset < mi->text_off || offset - mi->text_off >= mi->text_size || addr < g_code_lo ||
                addr >= g_code_hi) {
                WARN("hooks: %s lies outside the code of %s, which its build ID names, so it isn't hooked\n",
                     hook_symbol[k], mi->name);
                continue;
            }
            if (off[hook_family[k]] || site_of(addr) >= 0) continue;
            hook_put(k, addr, hook_symbol[k], verify);
            LOG("hook: %s at %llx (%s+%llx) runs %s\n", hook_symbol[k], (unsigned long long)addr, mi->name,
                (unsigned long long)offset, hook_handler[k]);
        }
}

u64 hook_site(const char* symbol) {
    const HostHook* h = &g_host.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (!strcmp(hook_symbol[h->hook[i]], symbol)) return h->addr[i];
    return 0;
}

/* A 32-bit game: its code is arm.c's blocks, and the hooks of its own table (hookgen --a32-marked: those declared
   a32, whose arguments and what their handlers read are the same under AAPCS32 and in the 32-bit SDK's layouts, and
   those declared for a 32-bit program alone) take their functions' blocks in the code table, a versioned one by the
   SDK version of the module that exports its function, as a 64-bit game's; no hook is verified */
static void install_a32(void) {
    const SnxModInfo* mods = g_arm_mod->mods;
    const char* name;
    u64 addr;
    u32 i, count = 0, nmods = g_arm_mod->nmods, sdk[HOOK_MODULES];
    unsigned char off[HOOK_FAMILIES + 1] = {0}, missed[HOOK32_COUNT + 1] = {0}, said[HOOK32_COUNT + 1] = {0};
    for (i = 0; i < HOOK_FAMILIES; i++) {
        const char* why = hook_family_unready(hook_family_name[i]);
        if ((off[i] = (unsigned char)family_listed(g_settings.hook_families_off, hook_family_name[i])) != 0)
            LOG("hooks: the %s family is off (hooks_off)\n", hook_family_name[i]);
        else if (why)
            off[i] = 1, LOG("hooks: the %s family is off: %s\n", hook_family_name[i], why);
    }
    module_versions(mods, nmods, sdk);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        u32 versions[HOOK_KEYS] = {0};
        SnxArmBlock* slot;
        int m, first, k;
        char range[96];
        if (!name || addr >> 32 || hook_select(&hook32_table, name, versions, &first) == -1 ||
            (m = module_of(mods, nmods, addr & ~1ull)) < 0)
            continue;
        versions[HOOK_SDK] = m < HOOK_MODULES ? sdk[m] : 0;
        k = hook_select(&hook32_table, name, versions, &first);
        if (k == -2 && !missed[first]) {
            char text[32];
            missed[first] = 1;
            missed_note(first, m, versions[hook32_key[first]]);
            hook_version_text(text, sizeof text, hook32_key[first], versions[hook32_key[first]]);
            LOG("hooks: %s in %s isn't hooked: no declaration for a 32-bit game holds for its version (%s)\n", name,
                mods[m].name, text);
        }
        if (k < 0 || off[hook32_family[k]]) continue;
        slot = arm_slot((u32)addr & ~1u, (int)(addr & 1));
        if (!slot || *slot == hook32_fn[k]) continue;
        if (g_host.hook.a32_count == HOOK_SITES)
            fatal("hooks: no site left for %s at %llx: the runtime holds %d, one for each hook, and the game's "
                  "modules export the functions of the hooks more than once",
                  name, (unsigned long long)addr, HOOK_SITES);
        /* the function's own block first: once the hook has its slot, the slot is all that names the address */
        g_host.hook.a32_orig[g_host.hook.a32_count] = arm_block((u32)addr & ~1u, (int)(addr & 1));
        g_host.hook.a32_addr[g_host.hook.a32_count++] = (u32)addr;
        __atomic_store_n(slot, hook32_fn[k], __ATOMIC_RELEASE);
        count++;
        if (hook32_key[k] == HOOK_ANY || said[k]) {
            LOG("hook: %s at %llx runs %s (32-bit)\n", name, (unsigned long long)addr, hook32_handler[k]);
            continue;
        }
        said[k] = 1; /* a versioned hook's versions and layouts, the first time it is installed */
        hook_range_text(range, sizeof range, hook32_key[k], hook32_lo[k], hook32_hi[k]);
        LOG("hook: %s at %llx runs %s (32-bit), the hook for %s%s%s\n", name, (unsigned long long)addr,
            hook32_handler[k], range, *hook32_layouts[k] ? ", with the layouts " : "", hook32_layouts[k]);
    }
    LOG("hooks: %u of the 32-bit game's functions run the runtime's handlers; a function of the SDK that no hook "
        "takes for a 32-bit game of its SDK yet runs its own code\n",
        (unsigned)count);
}

void hook_install(void) {
    const HostHook* h = &g_host.hook;
    const char* name;
    u64 addr;
    u32 i, sdk[HOOK_MODULES] = {0};
    unsigned char off[HOOK_FAMILIES + 1] = {0}, verify[HOOK_FAMILIES + 1] = {0};
    int any_verify = 0;
    unsigned char said[HOOK_COUNT + 1] = {0};   /* the entries whose versions the log gave */
    unsigned char missed[HOOK_COUNT + 1] = {0}; /* the symbols (by their first entry) the log said had none */
    g_host.hook.missed = 0;
    if (!g_settings.native_hooks) {
        LOG("hooks: off (hooks=0), every exported function runs its own code\n");
        return;
    }
    if (g_arm_mod) {
        install_a32();
        return;
    }
    if (!g_disp || !g_mod) return;
    routes_say();
    for (i = 0; i < HOOK_FAMILIES; i++)
        if ((off[i] = (unsigned char)family_listed(g_settings.hook_families_off, hook_family_name[i])) != 0)
            LOG("hooks: the %s family is off (hooks_off)\n", hook_family_name[i]);
        else if (hook_family_unready(hook_family_name[i]))
            off[i] = 1, LOG("hooks: the %s family is off: %s\n", hook_family_name[i],
                            hook_family_unready(hook_family_name[i]));
        else if (!strcmp(hook_family_name[i], LINKER_FAMILY) && !linker_links())
            off[i] = 1; /* nn::ro is the SDK's own while the game's linker runs: linker.c said why */
        else if (family_listed(g_settings.hook_families_verified, hook_family_name[i]))
            verify[i] = 1, any_verify = 1;
    if (any_verify) {
        g_hle.hook.verify_base = kmem_alloc((u64)HOOK_VERIFY_SLOTS * HOOK_VERIFY_SLOT, MS_KERNEL, PERM_RW);
        LOG("hooks: verify mode for the pure hooks of the families hooks_verify names, with %u KiB of scratch at %llx\n",
            (HOOK_VERIFY_SLOTS * HOOK_VERIFY_SLOT) >> 10, (unsigned long long)g_hle.hook.verify_base);
    }
    module_versions(g_mod->mods, g_mod->nmods, sdk);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        u32 versions[HOOK_KEYS] = {0};
        int m, first, k;
        char range[96];
        if (!name || addr < g_code_lo || addr >= g_code_hi || (addr & 3) ||
            hook_select(&hook_table, name, versions, &first) == -1 ||
            (m = module_of(g_mod->mods, g_mod->nmods, addr)) < 0 || site_of(addr) >= 0)
            continue;
        /* the versions of the module that exports it; the NVN API version isn't read yet, so it is unknown */
        versions[HOOK_SDK] = m < HOOK_MODULES ? sdk[m] : 0;
        k = hook_select(&hook_table, name, versions, &first);
        if (k == -2 && !missed[first]) {
            char text[32];
            missed[first] = 1;
            missed_note(first, m, versions[hook_key[first]]);
            hook_version_text(text, sizeof text, hook_key[first], versions[hook_key[first]]);
            LOG("hooks: %s in %s isn't hooked: no declaration holds for its version (%s)\n", name, g_mod->mods[m].name,
                text);
        }
        if (k < 0 || off[hook_family[k]]) continue;
        hook_put(k, addr, name, verify);
        if (hook_key[k] == HOOK_ANY || said[k]) {
            LOG("hook: %s at %llx runs %s\n", name, (unsigned long long)addr, hook_handler[k]);
            continue;
        }
        said[k] = 1; /* a versioned hook's versions and layouts, the first time it is installed */
        hook_range_text(range, sizeof range, hook_key[k], hook_lo[k], hook_hi[k]);
        LOG("hook: %s at %llx runs %s, the hook for %s%s%s\n", name, (unsigned long long)addr, hook_handler[k], range,
            *hook_layouts[k] ? ", with the layouts " : "", hook_layouts[k]);
    }
    hook_put_addresses(off, verify);
    LOG("hooks: %u of the game's functions run the runtime's handlers\n", h->count);
}

/* turns off the native hook of site i, whose function's first 4 bytes the game changed, and with it every site and
   entry whose adapter is the same code. Under g_invalidate_lock */
static PMutex g_invalidate_lock = PMUTEX_INIT;
static void hook_invalidate(u32 i) {
    HostHook* h = &g_host.hook;
    GuestFn adapter = hook_fn[h->hook[i]];
    char why[320];
    u32 j;
    int r, k;
    __atomic_store_n(&h->stale[i], 1, __ATOMIC_RELEASE); /* its own code is what the game put there */
    __atomic_store_n(&h->invalid[i], 1, __ATOMIC_RELEASE);
    for (j = 0; j < h->count; j++) /* an adapter another site's invalidation already rewrote */
        if (j != i && hook_fn[h->hook[j]] == adapter && __atomic_load_n(&h->invalid[j], __ATOMIC_ACQUIRE)) break;
    if (j == h->count && (r = snx_hook_jump_patch(adapter, hook_invalidated, why, sizeof why)) != 0) {
        if (r < 0)
            fatal("hooks: the game changed the first instruction of %s at %llx, which %s answers natively (a mod's "
                  "hook), and the native hook could not be turned off, so the game's code would never run: %s",
                  hook_symbol[h->hook[i]], (unsigned long long)h->addr[i], hook_handler[h->hook[i]], why);
        WARN("hooks: %s\n", why);
    }
    WARN("hooks: the game changed the first instruction of %s at %llx from %08x to %08x, which %s answered natively: "
         "a mod's hook. That native hook is off from here, and the game's code runs, the SDK's own code included "
         "wherever it calls it\n",
         hook_symbol[h->hook[i]], (unsigned long long)h->addr[i], snx_le32(h->word[i]),
         snx_le32(first_word(h->addr[i])), hook_handler[h->hook[i]]);
    /* the other sites of its adapter: their code did not change, so their slots run it, their own */
    for (j = 0; j < h->count; j++)
        if (j != i && hook_fn[h->hook[j]] == adapter && !__atomic_load_n(&h->invalid[j], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&h->invalid[j], 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_disp[(h->addr[j] - g_code_lo) >> 2], h->orig[j], __ATOMIC_RELEASE);
            WARN("hooks: %s at %llx shares that native hook's code, so it runs its own code from here too\n",
                 hook_symbol[h->hook[j]], (unsigned long long)h->addr[j]);
        }
    for (k = 0; k < HOOK_COUNT; k++) /* an entry whose adapter the linker folded into the same code */
        if (k != h->hook[i] && hook_fn[k] == adapter)
            WARN("hooks: the native hook of %s is the same code, so it is off too wherever it is installed\n",
                 hook_symbol[k]);
}

void hook_invalidated(CPU* RESTRICT c, u64 entry) {
    GuestFn own;
    if (site_of(entry) < 0)
        fatal("hooks: an invalidated native hook was called for %llx, where no hook was installed",
              (unsigned long long)entry);
    own = hook_original(entry);
    if (!own)
        fatal("hooks: the native hook at %llx is off, and nothing compiles the game's code there: the JIT is off",
              (unsigned long long)entry);
    own(c, entry);
}

void hook_restore(u64 addr, u64 size) {
    HostHook* h = &g_host.hook;
    MemBlock block;
    u64 end = size > ~(u64)0 - addr ? ~(u64)0 : addr + size;
    u32 i;
    for (i = 0; i < h->count; i++) {
        u64 a = h->addr[i];
        if (a < addr || a >= end) continue;
        mem_query(a, &block);
        if (block.state == MS_FREE || !(block.perm & PERM_X)) continue; /* no longer code: the hook goes with it */
        /* its first 4 bytes alone, which every mod's hook writes over: a change further in is not looked at */
        if (!__atomic_load_n(&h->invalid[i], __ATOMIC_ACQUIRE) && first_word(a) != h->word[i]) {
            pm_lock(&g_invalidate_lock);
            if (!__atomic_load_n(&h->invalid[i], __ATOMIC_ACQUIRE)) hook_invalidate(i);
            pm_unlock(&g_invalidate_lock);
        }
        if (__atomic_load_n(&h->invalid[i], __ATOMIC_ACQUIRE)) continue; /* its slot is the game's code's */
        __atomic_store_n(&g_disp[(a - g_code_lo) >> 2], h->fn[i], __ATOMIC_RELEASE);
    }
}

GuestFn hook_function(u64 addr) {
    int i = site_of(addr);
    return i < 0 || __atomic_load_n(&g_host.hook.invalid[i], __ATOMIC_ACQUIRE) ? NULL : g_host.hook.fn[i];
}

GuestFn hook_original(u64 addr) {
    int i = site_of(addr);
    if (i < 0) return NULL;
    if (!__atomic_load_n(&g_host.hook.stale[i], __ATOMIC_ACQUIRE)) return g_host.hook.orig[i];
    return jit_code_now(addr);
}

void hook_orig_stale(GuestFn f) {
    HostHook* h = &g_host.hook;
    u32 i;
    for (i = 0; i < h->count; i++)
        if (h->orig[i] == f) __atomic_store_n(&h->stale[i], 1, __ATOMIC_RELEASE);
}

const char* hook_at(u64 addr, const char** handler) {
    int i = site_of(addr);
    if (i < 0) return NULL;
    *handler = hook_handler[g_host.hook.hook[i]];
    return hook_symbol[g_host.hook.hook[i]];
}

void hook_run_original(CPU* c, u64 addr) {
    GuestFn own = hook_original(addr);
    u64 ret = c->x[30], sp = c->x[31];
    if (!own) fatal("hook: no hook at %llx to run the code of", (unsigned long long)addr);
    own(c, addr);
    while (c->pc != ret || c->x[31] != sp) snx_lookup(c, c->pc)(c, c->pc);
}

void hook_guest_call_a32(CPU* c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    SnxArmCPU* a = arm_cpu(c);
    u32 lr = a->r[14], at = a->r[15], state = a->cpsr & SNX_ARM_T;
    a->r[0] = a0;
    a->r[1] = a1;
    a->r[2] = a2;
    a->r[3] = a3;
    a->r[15] = fn & ~1u;
    a->cpsr = (a->cpsr & ~SNX_ARM_T) | (fn & 1 ? SNX_ARM_T : 0);
    arm_run(a, NULL, lr);
    a->r[14] = lr;
    a->r[15] = at;
    a->cpsr = (a->cpsr & ~SNX_ARM_T) | state;
}

u64 hook_run_original_a32(CPU* c) {
    SnxArmCPU* a = arm_cpu(c);
    u32 lr = a->r[14], at = a->r[15] | (a->cpsr & SNX_ARM_T ? 1u : 0u), i;
    for (i = 0; i < g_host.hook.a32_count && g_host.hook.a32_addr[i] != at; i++) {}
    if (i == g_host.hook.a32_count || !g_host.hook.a32_orig[i])
        fatal("hook: the function at %x has no code of its own to run", (unsigned)at);
    arm_run(a, g_host.hook.a32_orig[i], lr);
    a->r[14] = lr; /* the adapter returns through it, and the function's own code may leave anything there */
    return (u64)a->r[0] | (u64)a->r[1] << 32;
}
