/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct=1: the runtime runs the game's NVN calls itself (Part C of the NVN plan). A game gets every NVN function
   by name, from nvnBootstrapLoader and the nvnDeviceGetProcAddress it returns, and calls them through those
   pointers. Both are wrapped here as gpu/nvn_trace.c wraps them: the driver's own lookup still answers with the
   function's address, a search of its name table that touches nothing else, and that address's dispatch slot then
   holds the runtime's implementation (gpu/nvn/nvn.hook) instead of the driver's code. Every call the game makes
   through the pointer goes through the slot, so no instruction of the driver past its lookup runs, and the driver
   never sets the private flags (nvnPrivate0) that would send the game's inlined NVN code to write command memory
   itself (tools/nvn_census.py).
   A game's NVN objects are the runtime's from its first call, so nothing can be left to the driver: a function the
   game resolves that has no implementation yet gets a handler that stops the game with its name when it is called,
   which says what to implement next.

   The game's first nvnBootstrapLoader call, before it has any NVN function, reads the game's NVN API version: the
   driver's own lookup finds nvnDeviceGetInteger by name (it touches nothing else), and that function's switch answers
   it (gpu/nvn/nvn_driver.c). A function declared for some API versions (gpu/nvn/nvn.hook's @version(nvn ...)) takes
   the declaration that holds for it (hle/hook/version.c hook_select). With nvn_direct=2 (auto) the same call decides
   whether the runtime takes over at all: where the version is one nvn_direct has measured layouts for, it does, and
   otherwise the driver runs, the bootstrap loader's slot given back before the game gets its first function */
#include "nx/gpu/nvn/nvn_direct.h"
#include "nx/gpu/nvn/nvn_driver.h"
#include "nx/gpu/nvn/nvn_verify.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>

#include "gpu/nvn/nvn.hook.inc"

/* the functions the game resolved that aren't implemented: their addresses and names, for the message when one is
   called. Filled under lock, read by the handler of a call, which only runs once its entry was published */
typedef struct {
    u64 addr;
    char name[128];
} Missing;
#define MISSING_SLOTS 1024 /* a power of two above the ~530 functions NVN has */
/* the file's state: the missing functions, the lock their table and the slots are changed under, the resolvers'
   recompiled code, the game's NVN API version (nvn_api_pack, 0 unknown) once decided (under lock) and whether the
   runtime runs the game's NVN calls: from nvn_direct_init on, and with auto until the first bootstrap call says no */
typedef struct {
    Missing missing[MISSING_SLOTS];
    PMutex lock;
    GuestFn bootstrap_original, getproc_original;
    u64 bootstrap_entry;
    u64 flag; /* nvnPrivate0's address, 0 when the driver exports none */
    u32 api;
    int active, decided;
} NvnDirectState;
static NvnDirectState g_nvn_direct = {{{0}}, PMUTEX_INIT, 0, 0, 0, 0, 0, 0, 0};
static void getproc(CPU* c, u64 entry);
static void bootstrap(CPU* c, u64 entry);

int nvn_direct_active(void) { return __atomic_load_n(&g_nvn_direct.active, __ATOMIC_ACQUIRE); }

static u32 missing_hash(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (MISSING_SLOTS - 1); }
static const Missing* missing_find(u64 addr) {
    for (u32 n = 0, i = missing_hash(addr); n < MISSING_SLOTS; n++, i = (i + 1) & (MISSING_SLOTS - 1)) {
        u64 a = __atomic_load_n(&g_nvn_direct.missing[i].addr, __ATOMIC_ACQUIRE);
        if (a == addr) return &g_nvn_direct.missing[i];
        if (!a) return NULL;
    }
    return NULL;
}

static void unimplemented(CPU* c, u64 entry) {
    const Missing* m = missing_find(entry);
    (void)c;
    fatal("nvn: the game called %s, which nvn_direct doesn't implement yet", m ? m->name : "an NVN function");
}

/* whether a slot holds one of this file's handlers */
static int ours(GuestFn fn) {
    int i;
    if (fn == unimplemented || nvn_verify_ours(fn)) return 1;
    for (i = 0; i < NVN_COUNT; i++)
        if (nvn_fn[i] == fn) return 1;
    return 0;
}

/* name's entry in the generated table for the game's API version: its index, or -1 when none (*versioned: 1 when the
   name has entries, but none for this version) */
static int implemented(const char* name, int* versioned) {
    u32 versions[HOOK_KEYS] = {0};
    int first, k;
    versions[HOOK_NVN] = g_nvn_direct.api;
    k = hook_select(&nvn_table, name, versions, &first);
    *versioned = k == -2;
    if (k >= 0 && nvn_key[k] != HOOK_ANY) {
        char range[96];
        hook_range_text(range, sizeof range, nvn_key[k], nvn_lo[k], nvn_hi[k]);
        LOG_LIMIT(64, "nvn: %s runs %s, the one for %s\n", name, nvn_handler[k], range);
    }
    return k >= 0 ? k : -1;
}

/* the game resolved name to addr: its slot takes the runtime's implementation, or the handler that names it */
static void take(u64 addr, const char* name) {
    int versioned, i = implemented(name, &versioned);
    GuestFn fn = i >= 0 ? nvn_fn[i] : unimplemented, *slot;
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3))
        fatal("nvn: the driver resolved %s to %llx, outside the game's code, where nvn_direct can't take its calls",
              name, (unsigned long long)addr);
    if (!strcmp(name, "nvnDeviceGetInteger")) nvn_device_driver(addr); /* its answers, from its code */
    pm_lock(&g_nvn_direct.lock);
    slot = &g_disp[(addr - g_code_lo) / 4];
    if (*slot == getproc || *slot == bootstrap) { /* the resolvers, which a game also looks up by name: still wrapped */
        pm_unlock(&g_nvn_direct.lock);
        return;
    }
    if (*slot != fn && ours(*slot)) {
        /* two names the driver folded to one address, identical code: the first implemented one keeps it */
        LOG_LIMIT(16, "nvn: %s shares its address with a function resolved before it\n", name);
        if (*slot != unimplemented || fn == unimplemented) {
            pm_unlock(&g_nvn_direct.lock);
            return;
        }
    }
    if (fn == unimplemented && !missing_find(addr)) {
        u32 j = missing_hash(addr);
        char version[32];
        while (g_nvn_direct.missing[j].addr) j = (j + 1) & (MISSING_SLOTS - 1);
        hook_version_text(version, sizeof version, HOOK_NVN, g_nvn_direct.api);
        if (versioned) /* its declarations are for other API versions */
            snprintf(g_nvn_direct.missing[j].name, sizeof g_nvn_direct.missing[j].name, "%s for NVN API %s", name,
                     version);
        else
            snprintf(g_nvn_direct.missing[j].name, sizeof g_nvn_direct.missing[j].name, "%s", name);
        LOG("nvn: the game resolved %s, which nvn_direct doesn't implement: it stops if it calls it\n",
            g_nvn_direct.missing[j].name);
        __atomic_store_n(&g_nvn_direct.missing[j].addr, addr, __ATOMIC_RELEASE);
    }
    /* verify mode: the driver's code, which the slot holds until now, runs after the handler on shadows */
    if (g_settings.nvn_verify) fn = nvn_verify_take(addr, name, fn, ours(*slot) ? NULL : *slot, fn != unimplemented);
    __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    pm_unlock(&g_nvn_direct.lock);
}

/* The driver's lookup runs with no device. Given one, it reads the driver's own state in it, which the runtime's
   device doesn't have, and sets nvnPrivate0 when no debug layer is installed. That flag sends the game's inlined NVN
   code to write Maxwell methods into command memory itself, through the command buffer's first words, which are the
   runtime's state. With no device the lookup answers the same address and leaves the flag 0 (one game's driver: its
   lookup stores device && no debug layer in it) */
static void getproc(CPU* c, u64 entry) {
    char name[128];
    int valid;
    if (nvn_verify_shadow_thread()) { /* the shadow driver's own lookups, with its device: as they are */
        g_nvn_direct.getproc_original(c, entry);
        return;
    }
    valid = nvn_read_name(c->x[1], name); /* nvnDeviceGetProcAddress(device, name) */
    c->x[0] = 0;
    g_nvn_direct.getproc_original(c, entry);
    if (g_nvn_direct.flag && gr32(g_nvn_direct.flag))
        fatal("nvn: the driver's lookup of %s set nvnPrivate0, which turns on the game's inlined command writes", name);
    if (valid && c->x[0]) take(c->x[0], name);
}

/* The driver's own lookup of name, run on c from inside the game's bootstrap call (entry: the loader's address) as
   nvn_cmd.c runs a callback: the name goes below the stack pointer, the call runs until it returns to the game's
   return address, which nothing else returns to, and the game's x0, link register and stack pointer are put back. The
   function's address, 0 for none */
static u64 driver_lookup(CPU* c, u64 entry, const char* name) {
    size_t n = strlen(name) + 1;
    u64 sp = c->x[31], lr = c->x[30], arg = c->x[0], at = (sp - n) & ~15ull, pc, found;
    memcpy(GP(at), name, n);
    c->x[31] = at;
    c->x[0] = at;
    g_nvn_direct.bootstrap_original(c, entry);
    while ((pc = c->pc) != lr) snx_lookup(c, pc)(c, pc);
    found = c->x[0];
    c->x[31] = sp;
    c->x[30] = lr;
    c->x[0] = arg;
    c->pc = lr;
    return found;
}

/* Once, at the game's first bootstrap call, before it has any NVN function: the game's NVN API version, from its
   driver's nvnDeviceGetInteger, and with auto whether the runtime takes the game's NVN calls. The inlined command
   writes a game may make (tools/nvn_census.py) run only while the driver's nvnPrivate0 is set, which only the
   driver's lookup given a device does, and getproc never gives it one: under nvn_direct the flag stays 0 whatever the
   game imports, so the version alone decides. Under lock */
static void decide(CPU* c, u64 entry) {
    u64 f = driver_lookup(c, entry, "nvnDeviceGetInteger");
    char version[32], described_text[64];
    int described;
    if (f >= g_code_lo && f < g_code_hi && !(f & 3)) nvn_device_driver(f); /* its answers, from its code */
    g_nvn_direct.api = nvn_device_api();
    described = nvn_api_described(g_nvn_direct.api);
    hook_version_text(version, sizeof version, HOOK_NVN, g_nvn_direct.api);
    nvn_api_described_text(described_text, sizeof described_text);
    g_nvn_direct.decided = 1;
    if (g_settings.nvn_direct != 2) {
        if (!described)
            WARN("nvn: nvn_direct=1 with the game's NVN API %s, which nvn_direct has no measured layouts for (%s)\n",
                 version, described_text);
        return;
    }
    if (described) {
        LOG("nvn: nvn_direct=auto: the runtime runs the game's NVN calls: its NVN API %s is one nvn_direct has "
            "measured layouts for (%s), and nvnPrivate0, which turns on its inlined command writes, stays 0 (%s)\n",
            version, described_text,
            g_nvn_direct.flag ? "the driver's lookup runs without a device" : "the driver exports none");
        return;
    }
    LOG("nvn: nvn_direct=auto: the game's NVN driver runs: its NVN API %s isn't one nvn_direct has measured layouts "
        "for (%s)\n",
        version, described_text);
    __atomic_store_n(&g_disp[(entry - g_code_lo) / 4], g_nvn_direct.bootstrap_original, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nvn_direct.active, 0, __ATOMIC_RELEASE);
}

static void bootstrap(CPU* c, u64 entry) {
    char name[128];
    int valid;
    GuestFn* slot;
    if (nvn_verify_shadow_thread()) {
        g_nvn_direct.bootstrap_original(c, entry);
        return;
    }
    valid = nvn_read_name(c->x[0], name); /* nvnBootstrapLoader(name) */
    pm_lock(&g_nvn_direct.lock);
    if (!g_nvn_direct.decided) decide(c, entry);
    pm_unlock(&g_nvn_direct.lock);
    g_nvn_direct.bootstrap_original(c, entry);
    if (!nvn_direct_active()) return; /* auto chose the driver: it runs from here on */
    if (!valid || !c->x[0]) return;
    if (strcmp(name, "nvnDeviceGetProcAddress")) {
        take(c->x[0], name);
        return;
    }
    /* the resolver: wrapped, so the names it answers are taken too */
    if (c->x[0] < g_code_lo || c->x[0] >= g_code_hi || (c->x[0] & 3))
        fatal("nvn: the driver's nvnDeviceGetProcAddress is at %llx, outside the game's code",
              (unsigned long long)c->x[0]);
    pm_lock(&g_nvn_direct.lock);
    slot = &g_disp[(c->x[0] - g_code_lo) / 4];
    if (*slot != getproc) {
        if (!*slot) fatal("nvn: the driver's nvnDeviceGetProcAddress has no recompiled code");
        g_nvn_direct.getproc_original = *slot;
        __atomic_store_n(slot, getproc, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_nvn_direct.lock);
}

void nvn_direct_init(void) {
    u64 entry;
    GuestFn* slot;
    int automatic = g_settings.nvn_direct == 2;
    if (!g_settings.nvn_direct) return;
    if (automatic && g_settings.nvn_trace) {
        LOG("nvn: nvn_direct=auto: the game's NVN driver runs, for nvn_trace to trace\n");
        return;
    }
    entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    if (automatic && (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3) ||
                      !g_disp[(entry - g_code_lo) / 4])) {
        LOG("nvn: nvn_direct=auto: the game has no recompiled nvnBootstrapLoader, so no NVN to take\n");
        return;
    }
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3))
        fatal("nvn: nvn_direct needs the game's nvnBootstrapLoader, which none of its executables exports");
    slot = &g_disp[(entry - g_code_lo) / 4];
    if (!*slot) fatal("nvn: the game's nvnBootstrapLoader has no recompiled code");
    g_nvn_direct.bootstrap_original = *slot;
    g_nvn_direct.bootstrap_entry = entry;
    g_nvn_direct.flag = kernel_find_symbol_exact("nvnPrivate0");
    *slot = bootstrap; /* before any guest thread runs */
    g_nvn_direct.active = 1;
    if (automatic)
        LOG("nvn: nvn_direct=auto: the game's NVN API version decides at its first NVN call whether the runtime runs "
            "them (%d implemented)\n",
            NVN_COUNT);
    else
        LOG("nvn: nvn_direct: the runtime implements %d NVN functions, and the game's NVN driver doesn't run\n",
            NVN_COUNT);
}

u64 nvn_direct_lookup(CPU* c, const char* name) {
    return g_nvn_direct.bootstrap_original ? driver_lookup(c, g_nvn_direct.bootstrap_entry, name) : 0;
}
u32 nvn_direct_flag(void) { return g_nvn_direct.flag ? gr32(g_nvn_direct.flag) : 0; }
void nvn_direct_flag_set(u32 v) {
    if (g_nvn_direct.flag) snx_st_le32(GP(g_nvn_direct.flag), v);
}
