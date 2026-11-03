/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN's loader: the runtime runs the game's NVN calls itself (Part C of the NVN plan), and that is the GPU path. A game
   gets every NVN function by name, from nvnBootstrapLoader and the nvnDeviceGetProcAddress it returns, and calls them
   through those pointers. Both are wrapped here: the driver's own lookup still answers with the function's address, a
   search of its name table that touches nothing else, and that address's dispatch slot then holds the runtime's
   implementation (gpu/nvn/nvn.hook) instead of the driver's code. Every call the game makes through the pointer goes
   through the slot, so no instruction of the driver past its lookup runs, and the driver never sets the private flags
   (nvnPrivate0) that would send the game's inlined NVN code to write command memory itself (tools/nvn_census.py). A
   game's NVN objects are the runtime's from its first call, so nothing can be left to the driver: a function the game
   resolves that has no implementation yet gets a handler that stops the game with its name when it is called, which
   says what to implement next.

   The game's first nvnBootstrapLoader call, before it has any NVN function, reads the game's NVN API version: the
   driver's own lookup finds nvnDeviceGetInteger by name (it touches nothing else), and that function's switch answers
   it (gpu/nvn/nvn_driver.c). A function declared for some API versions (gpu/nvn/nvn.hook's @version(nvn ...)) takes the
   declaration that holds for it (hle/hook/version.c hook_select). The log says whether the version is one the runtime
   has measured layouts for: the runtime takes the game's NVN calls either way, as the driver has no GPU to run on */
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/nvn/nvn_loader.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_driver.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>

#include "gpu/nvn/nvn.hook.inc"

/* a NUL-terminated name of at most 127 characters in guest memory, read a page at a time (memory permissions are per
   page): its length, or 0 when it is empty, unreadable or longer */
static int readable(u64 addr) {
    if (addr < AS_BASE || addr >= AS_SIZE) return 0;
    MemBlock block;
    mem_query(addr, &block);
    return block.state != MS_FREE && block.state != MS_INACCESSIBLE && (block.perm & PERM_R);
}
static u32 nvn_read_name(u64 ptr, char name[128]) {
    u64 readable_end = 0;
    size_t i;
    for (i = 0; i < 127 && ptr + i >= ptr; i++) {
        u64 at = ptr + i;
        if (at >= readable_end) {
            if (!readable(at)) break;
            readable_end = (at | 0xFFF) + 1;
        }
        name[i] = *(const char*)GP(at);
        if (!name[i]) return (u32)i;
    }
    name[i] = 0;
    return 0;
}

static void getproc(CPU* c, u64 entry);
static void bootstrap(CPU* c, u64 entry);
static void getproc32(SnxArmCPU* a);
static void bootstrap32(SnxArmCPU* a);

int nvn_loader_active(void) { return __atomic_load_n(&g_nvn.loader.active, __ATOMIC_ACQUIRE); }

static u32 missing_hash(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (NVN_MISSING_SLOTS - 1); }
static const NvnMissing* missing_find(u64 addr) {
    for (u32 n = 0, i = missing_hash(addr); n < NVN_MISSING_SLOTS; n++, i = (i + 1) & (NVN_MISSING_SLOTS - 1)) {
        u64 a = __atomic_load_n(&g_nvn.loader.missing[i].addr, __ATOMIC_ACQUIRE);
        if (a == addr) return &g_nvn.loader.missing[i];
        if (!a) return NULL;
    }
    return NULL;
}

static void unimplemented(CPU* c, u64 entry) {
    const NvnMissing* m = missing_find(entry);
    (void)c;
    fatal("nvn: the game called %s, which nvn doesn't implement yet", m ? m->name : "an NVN function");
}

/* a 32-bit game's: the block at the function's entry, so r15 is its address (bit 0 set in Thumb state) */
static void unimplemented32(SnxArmCPU* a) {
    const NvnMissing* m = missing_find(a->r[15] | (a->cpsr & SNX_ARM_T ? 1u : 0u));
    fatal("nvn: the game called %s, which nvn doesn't implement yet", m ? m->name : "an NVN function");
}
static int ours32(SnxArmBlock fn) {
    int i;
    if (fn == unimplemented32) return 1;
    for (i = 0; i < NVN_COUNT; i++)
        if (nvn_a32[i] == fn) return 1;
    return 0;
}

/* whether a slot holds one of this file's handlers */
static int ours(GuestFn fn) {
    int i;
    if (fn == unimplemented) return 1;
    for (i = 0; i < NVN_COUNT; i++)
        if (nvn_fn[i] == fn) return 1;
    return 0;
}

/* name's entry in the generated table for the game's API version: its index, or -1 when none (*versioned: 1 when the
   name has entries, but none for this version) */
static int implemented(const char* name, int* versioned) {
    u32 versions[HOOK_KEYS] = {0};
    int first, k;
    versions[HOOK_NVN] = g_nvn.loader.api;
    k = hook_select(&nvn_table, name, versions, &first);
    *versioned = k == -2;
    if (k >= 0 && nvn_key[k] != HOOK_ANY) {
        char range[96];
        hook_range_text(range, sizeof range, nvn_key[k], nvn_lo[k], nvn_hi[k]);
        LOG_LIMIT(64, "nvn: %s runs %s, the one for %s\n", name, nvn_handler[k], range);
    }
    return k >= 0 ? k : -1;
}

/* addr's name for unimplemented's message, under lock (versioned: it has declarations for other API versions) */
static void note_missing(u64 addr, const char* name, int versioned) {
    u32 j = missing_hash(addr);
    char version[32];
    while (g_nvn.loader.missing[j].addr) j = (j + 1) & (NVN_MISSING_SLOTS - 1);
    hook_version_text(version, sizeof version, HOOK_NVN, g_nvn.loader.api);
    if (versioned)
        snprintf(g_nvn.loader.missing[j].name, sizeof g_nvn.loader.missing[j].name, "%s for NVN API %s", name, version);
    else
        snprintf(g_nvn.loader.missing[j].name, sizeof g_nvn.loader.missing[j].name, "%s", name);
    LOG("nvn: the game resolved %s, which nvn doesn't implement: it stops if it calls it\n",
        g_nvn.loader.missing[j].name);
    __atomic_store_n(&g_nvn.loader.missing[j].addr, addr, __ATOMIC_RELEASE);
}

/* take for a 32-bit game: addr is its A32 address, or its Thumb one with bit 0 set */
static void take32(u64 addr, const char* name, int i, int versioned) {
    SnxArmBlock fn = i >= 0 ? nvn_a32[i] : unimplemented32,
                *slot = addr >> 32 ? NULL : arm_slot((u32)addr & ~1u, addr & 1);
    if (!slot)
        fatal("nvn: the driver resolved %s to %llx, outside the game's code, where nvn can't take its calls",
              name, (unsigned long long)addr);
    pm_lock(&g_nvn.loader.lock);
    if (*slot == getproc32 || *slot == bootstrap32) {
        pm_unlock(&g_nvn.loader.lock);
        return;
    }
    if (*slot != fn && ours32(*slot)) {
        LOG_LIMIT(16, "nvn: %s shares its address with a function resolved before it\n", name);
        if (*slot != unimplemented32 || fn == unimplemented32) {
            pm_unlock(&g_nvn.loader.lock);
            return;
        }
    }
    if (fn == unimplemented32 && !missing_find(addr)) note_missing(addr, name, versioned);
    __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    pm_unlock(&g_nvn.loader.lock);
}

/* the game resolved name (len characters) to addr: its slot takes the runtime's implementation, or the handler that
   names it */
static void take(u64 addr, const char* name, u32 len) {
    int versioned, i = implemented(name, &versioned);
    GuestFn fn = i >= 0 ? nvn_fn[i] : unimplemented, *slot;
    if (g_nvn.loader.a32) {
        if (!strcmp(name, "nvnDeviceGetInteger")) nvn_device_driver(addr); /* its answers, from its code */
        take32(addr, name, i, versioned);
        return;
    }
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3))
        fatal("nvn: the driver resolved %s to %llx, outside the game's code, where nvn can't take its calls",
              name, (unsigned long long)addr);
    if (!strcmp(name, "nvnDeviceGetInteger")) nvn_device_driver(addr); /* its answers, from its code */
    pm_lock(&g_nvn.loader.lock);
    slot = &g_disp[(addr - g_code_lo) / 4];
    if (*slot == getproc || *slot == bootstrap) { /* the resolvers, which a game also looks up by name: still wrapped */
        pm_unlock(&g_nvn.loader.lock);
        return;
    }
    if (*slot != fn && ours(*slot)) {
        /* two names the driver folded to one address, identical code: the first implemented one keeps it */
        LOG_LIMIT(16, "nvn: %s shares its address with a function resolved before it\n", name);
        if (*slot != unimplemented || fn == unimplemented) {
            pm_unlock(&g_nvn.loader.lock);
            return;
        }
    }
    /* the NvRm sync conversions run the driver's own code on a sync in its layout (nvn_object.c): it is the slot's
       until the handler takes it */
    if (!ours(*slot) && !strcmp(name, "nvnSyncGetNvRmSyncNVX"))
        g_nvn.loader.nvrm_get = *slot, g_nvn.loader.nvrm_get_at = addr;
    if (!ours(*slot) && !strcmp(name, "nvnSyncInitializeFromNvRmSyncNVX"))
        g_nvn.loader.nvrm_init = *slot, g_nvn.loader.nvrm_init_at = addr;
    if (fn == unimplemented && !missing_find(addr)) note_missing(addr, name, versioned);
    __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    pm_unlock(&g_nvn.loader.lock);
}

/* The driver's lookup runs with no device. Given one, it reads the driver's own state in it, which the runtime's
   device doesn't have, and sets nvnPrivate0 when no debug layer is installed. That flag sends the game's inlined NVN
   code to write Maxwell methods into command memory itself, through the command buffer's first words, which are the
   runtime's state. With no device the lookup answers the same address and leaves the flag 0 (one game's driver: its
   lookup stores device && no debug layer in it) */
static void getproc(CPU* c, u64 entry) {
    char name[128];
    u32 len = nvn_read_name(c->x[1], name); /* nvnDeviceGetProcAddress(device, name) */
    c->x[0] = 0;
    g_nvn.loader.getproc_original(c, entry);
    if (g_nvn.loader.flag && gr32(g_nvn.loader.flag))
        fatal("nvn: the driver's lookup of %s set nvnPrivate0, which turns on the game's inlined command writes", name);
    if (len && c->x[0]) take(c->x[0], name, len);
}

u64 nvn_loader_run_driver(CPU* c, GuestFn fn, u64 at, u64 a0, u64 a1, u64 a2) {
    u64 lr = c->x[30], pc;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    fn(c, at);
    while ((pc = c->pc) != lr) snx_lookup(c, pc)(c, pc);
    c->x[30] = lr;
    return c->x[0];
}

/* The driver's own lookup of name (n characters), run on c from inside the game's bootstrap call (entry: the loader's address) as
   nvn_cmd.c runs a callback: the name goes below the stack pointer, the call runs until it returns to the game's
   return address, which nothing else returns to, and the game's x0, link register and stack pointer are put back. The
   function's address, 0 for none */
static u64 driver_lookup(CPU* c, u64 entry, const char* name, size_t n) {
    u64 sp = c->x[31], lr = c->x[30], arg = c->x[0], at = (sp - (n + 1)) & ~15ull, pc, found;
    memcpy(GP(at), name, n);
    *(char*)GP(at + n) = 0;
    c->x[31] = at;
    c->x[0] = at;
    g_nvn.loader.bootstrap_original(c, entry);
    while ((pc = c->pc) != lr) snx_lookup(c, pc)(c, pc);
    found = c->x[0];
    c->x[31] = sp;
    c->x[30] = lr;
    c->x[0] = arg;
    c->pc = lr;
    return found;
}

/* Once, at the game's first bootstrap call, before it has any NVN function: the game's NVN API version, from its
   driver's nvnDeviceGetInteger. The inlined command writes a game may make (tools/nvn_census.py) run only while the
   driver's nvnPrivate0 is set, which only the driver's lookup given a device does, and getproc never gives it one:
   the flag stays 0 whatever the game imports. Under lock */
static const char get_integer[] = "nvnDeviceGetInteger";
static void version_read(void);
static void version_find(CPU* c, u64 entry) {
    u64 f = driver_lookup(c, entry, get_integer, sizeof get_integer - 1);
    if (f >= g_code_lo && f < g_code_hi && !(f & 3)) nvn_device_driver(f); /* its answers, from its code */
    version_read();
}
/* what version_find does once the driver's answers are read: the version, and the log line that says whether the
   runtime has measured layouts for it */
static void version_read(void) {
    char version[32], described_text[64];
    g_nvn.loader.api = nvn_device_api();
    /* an Ounce GPU's: SM70+ programs and the T239's GOB, as the NPDM said already for an Ounce game (g_ounce) */
    if (g_nvn.loader.api >= 231u << 16) g_shader_sm70 = g_gob_native = 1;
    hook_version_text(version, sizeof version, HOOK_NVN, g_nvn.loader.api);
    nvn_api_described_text(described_text, sizeof described_text);
    g_nvn.loader.decided = 1;
    if (nvn_api_described(g_nvn.loader.api))
        LOG("nvn: the game's NVN API %s is one the runtime has measured layouts for (%s), and nvnPrivate0, which "
            "turns on its inlined command writes, stays 0 (%s)\n",
            version, described_text,
            g_nvn.loader.flag ? "the driver's lookup runs without a device" : "the driver exports none");
    else
        LOG("nvn: the game's NVN API %s isn't one the runtime has measured layouts for (%s): the runtime runs its NVN "
            "calls all the same\n",
            version, described_text);
}

static void bootstrap(CPU* c, u64 entry) {
    GuestFn* slot;
    char name[128];
    u32 len = nvn_read_name(c->x[0], name); /* nvnBootstrapLoader(name) */
    pm_lock(&g_nvn.loader.lock);
    if (!g_nvn.loader.decided) version_find(c, entry);
    pm_unlock(&g_nvn.loader.lock);
    g_nvn.loader.bootstrap_original(c, entry);
    if (!len || !c->x[0]) return;
    if (strcmp(name, "nvnDeviceGetProcAddress")) {
        take(c->x[0], name, len);
        return;
    }
    /* the resolver: wrapped, so the names it answers are taken too */
    if (c->x[0] < g_code_lo || c->x[0] >= g_code_hi || (c->x[0] & 3))
        fatal("nvn: the driver's nvnDeviceGetProcAddress is at %llx, outside the game's code",
              (unsigned long long)c->x[0]);
    pm_lock(&g_nvn.loader.lock);
    slot = &g_disp[(c->x[0] - g_code_lo) / 4];
    if (*slot != getproc) {
        if (!*slot) fatal("nvn: the driver's nvnDeviceGetProcAddress has no recompiled code");
        g_nvn.loader.getproc_original = *slot;
        __atomic_store_n(slot, getproc, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_nvn.loader.lock);
}

/* ---- a 32-bit game (nx/runtime/arm.c): its driver is A32 code, and its NVN functions are blocks of its code table,
   each called by a branch that ends the caller's block, so a block put in a function's slot takes every call ---- */

/* the A32 state a block at addr (bit 0 for Thumb) is entered in, as a branch to it sets it */
static void a32_at(SnxArmCPU* a, u64 addr) {
    a->r[15] = (u32)addr & ~1u;
    a->cpsr = (a->cpsr & ~SNX_ARM_T) | (addr & 1 ? SNX_ARM_T : 0);
}

/* driver_lookup on a 32-bit game's thread, from inside its bootstrap call: the name below the stack pointer, the
   driver's loader run until it returns to the call's return address, and the call's registers put back as they were
   at its entry */
static u64 driver_lookup32(SnxArmCPU* a, const char* name, size_t n) {
    u32 sp = a->r[13], lr = a->r[14], arg = a->r[0], at = (sp - (u32)(n + 1)) & ~7u;
    u64 found;
    memcpy(GP(at), name, n);
    *(char*)GP(at + n) = 0;
    a->r[13] = at;
    a->r[0] = at;
    arm_run(a, g_nvn.loader.bootstrap_original32, lr);
    found = a->r[0];
    a->r[13] = sp;
    a->r[14] = lr;
    a->r[0] = arg;
    a32_at(a, g_nvn.loader.bootstrap_entry);
    return found;
}

/* getproc for a 32-bit game: the driver's lookup with no device (r0), run until it returns to the caller */
static void getproc32(SnxArmCPU* a) {
    char name[128];
    u32 len = nvn_read_name(a->r[1], name); /* nvnDeviceGetProcAddress(device, name) */
    a->r[0] = 0;
    arm_run(a, g_nvn.loader.getproc_original32, a->r[14]);
    if (g_nvn.loader.flag && gr32(g_nvn.loader.flag))
        fatal("nvn: the driver's lookup of %s set nvnPrivate0, which turns on the game's inlined command writes", name);
    if (len && a->r[0]) take(a->r[0], name, len);
}

static void bootstrap32(SnxArmCPU* a) {
    SnxArmBlock* slot;
    char name[128];
    u32 len = nvn_read_name(a->r[0], name), found; /* nvnBootstrapLoader(name) */
    pm_lock(&g_nvn.loader.lock);
    if (!g_nvn.loader.decided) {
        u64 f = driver_lookup32(a, get_integer, sizeof get_integer - 1);
        if (f && !(f >> 32) && arm_slot((u32)f & ~1u, f & 1)) nvn_device_driver(f); /* its answers, from its code */
        version_read();
    }
    pm_unlock(&g_nvn.loader.lock);
    arm_run(a, g_nvn.loader.bootstrap_original32, a->r[14]);
    found = a->r[0];
    if (!len || !found) return;
    if (strcmp(name, "nvnDeviceGetProcAddress")) {
        take(found, name, len);
        return;
    }
    /* the resolver: wrapped, so the names it answers are taken too */
    slot = arm_slot(found & ~1u, found & 1);
    if (!slot) fatal("nvn: the driver's nvnDeviceGetProcAddress is at %x, outside the game's code", found);
    pm_lock(&g_nvn.loader.lock);
    if (*slot != getproc32) {
        g_nvn.loader.getproc_original32 = arm_block(found & ~1u, found & 1);
        if (!g_nvn.loader.getproc_original32)
            fatal("nvn: the driver's nvnDeviceGetProcAddress at %x has no code", found);
        __atomic_store_n(slot, getproc32, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_nvn.loader.lock);
}

/* nvn_loader_init for a 32-bit game. Every call to its loader ends a block, so no route list is needed */
static void init32(void) {
    u64 entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    SnxArmBlock* slot = entry && !(entry >> 32) ? arm_slot((u32)entry & ~1u, entry & 1) : NULL;
    if (!slot) {
        LOG("nvn: the game has no nvnBootstrapLoader in its code, so no NVN to take: it draws nothing\n");
        return;
    }
    g_nvn.loader.a32 = 1;
    g_nvn.loader.bootstrap_original32 = arm_block((u32)entry & ~1u, entry & 1);
    if (!g_nvn.loader.bootstrap_original32)
        fatal("nvn: the game's nvnBootstrapLoader at %llx has no code", (unsigned long long)entry);
    g_nvn.loader.bootstrap_entry = entry;
    g_nvn.loader.flag = kernel_find_symbol_exact("nvnPrivate0");
    *slot = bootstrap32; /* before any guest thread runs */
    g_nvn.loader.active = 1;
    LOG("nvn: the runtime implements %d NVN functions for the game's 32-bit code\n", NVN_COUNT);
}

void nvn_loader_init(void) {
    u64 entry;
    GuestFn* slot;
    if (g_arm_mod) {
        init32();
        return;
    }
    entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3) || !g_disp[(entry - g_code_lo) / 4]) {
        LOG("nvn: the game has no recompiled nvnBootstrapLoader, so no NVN to take: it draws nothing\n");
        return;
    }
    slot = &g_disp[(entry - g_code_lo) / 4];
    g_nvn.loader.bootstrap_original = *slot;
    g_nvn.loader.bootstrap_entry = entry;
    g_nvn.loader.flag = kernel_find_symbol_exact("nvnPrivate0");
    if (!hook_routed("nvnBootstrapLoader"))
        WARN("nvn: the module's route list doesn't name nvnBootstrapLoader, so nvn sees only its calls through "
             "the PLT and function pointers. Recompile the game for its direct calls\n");
    *slot = bootstrap; /* before any guest thread runs */
    g_nvn.loader.active = 1;
    LOG("nvn: the runtime implements %d NVN functions, and the game's NVN driver doesn't run\n", NVN_COUNT);
}
