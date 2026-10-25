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
   which says what to implement next */
#include "nx/gpu/nvn/nvn_direct.h"
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
/* the file's state */
typedef struct {
    Missing missing[MISSING_SLOTS];
    PMutex lock;
    GuestFn bootstrap_original, getproc_original;
    int active;
} NvnDirectState;
static NvnDirectState g_nvn_direct = {{{0}}, PMUTEX_INIT, 0, 0, 0};
static void getproc(CPU* c, u64 entry);
static void bootstrap(CPU* c, u64 entry);

int nvn_direct_active(void) { return g_nvn_direct.active; }

/* the generated table's index of name (sorted by name), or -1 */
static int implemented(const char* name) {
    int lo = 0, hi = NVN_COUNT;
    while (lo < hi) {
        int mid = (lo + hi) / 2, d = strcmp(nvn_symbol[mid], name);
        if (!d) return mid;
        if (d < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

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
    if (fn == unimplemented) return 1;
    for (i = 0; i < NVN_COUNT; i++)
        if (nvn_fn[i] == fn) return 1;
    return 0;
}

/* the game resolved name to addr: its slot takes the runtime's implementation, or the handler that names it */
static void take(u64 addr, const char* name) {
    int i = implemented(name);
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
        while (g_nvn_direct.missing[j].addr) j = (j + 1) & (MISSING_SLOTS - 1);
        memcpy(g_nvn_direct.missing[j].name, name, strlen(name) + 1);
        __atomic_store_n(&g_nvn_direct.missing[j].addr, addr, __ATOMIC_RELEASE);
    }
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
    int valid = nvn_read_name(c->x[1], name); /* nvnDeviceGetProcAddress(device, name) */
    c->x[0] = 0;
    g_nvn_direct.getproc_original(c, entry);
    if (valid && c->x[0]) take(c->x[0], name);
}

static void bootstrap(CPU* c, u64 entry) {
    char name[128];
    int valid = nvn_read_name(c->x[0], name); /* nvnBootstrapLoader(name) */
    GuestFn* slot;
    g_nvn_direct.bootstrap_original(c, entry);
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
    if (!g_settings.nvn_direct) return;
    entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3))
        fatal("nvn: nvn_direct needs the game's nvnBootstrapLoader, which none of its executables exports");
    slot = &g_disp[(entry - g_code_lo) / 4];
    if (!*slot) fatal("nvn: the game's nvnBootstrapLoader has no recompiled code");
    g_nvn_direct.bootstrap_original = *slot;
    *slot = bootstrap; /* before any guest thread runs */
    g_nvn_direct.active = 1;
    LOG("nvn: nvn_direct: the runtime implements %d NVN functions, and the game's NVN driver doesn't run\n",
        NVN_COUNT);
}
