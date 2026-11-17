/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Armature in the runtime (armature.h): the library and its C ABI (armature's src/ffi.rs), the copies of the guest
   registers between CPU and Armature's context, its callbacks, the traps of the dispatch slots and the kicks */
#include "nx/runtime/armature.h"
#include "common/utilities/endian.h"
#include "common/platform/dl.h"
#include "nx/hle/hle.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "snx_settings.h"
#include <string.h>

/* armature's ArmatureMemoryCallbacks, ArmatureExceptionCallbacks, ArmatureHostTraps and ArmatureRegs (ffi.rs) */
typedef struct ArmMemory {
    void* ctx;
    int32_t address_space_bits;
    int32_t manager_type; /* 3: HostMappedUnsafe, the guest's address added to page_table_pointer */
    u8* page_table_pointer;
    u8 (*read_u8)(void*, u64);
    u16 (*read_u16)(void*, u64);
    u32 (*read_u32)(void*, u64);
    u64 (*read_u64)(void*, u64);
    void (*read_v128)(void*, u64, u8*);
    void (*write_u8)(void*, u64, u8);
    void (*write_u16)(void*, u64, u16);
    void (*write_u32)(void*, u64, u32);
    void (*write_u64)(void*, u64, u64);
    void (*write_v128)(void*, u64, const u8*);
    u8 (*is_mapped)(void*, u64);
    void (*signal_memory_tracking)(void*, u64, u64, u8);
} ArmMemory;
typedef struct ArmExceptions {
    void* ctx;
    void (*on_break)(void*, void*, u64, int32_t);
    void (*on_supervisor_call)(void*, void*, u64, int32_t);
    void (*on_undefined)(void*, void*, u64, int32_t);
    void (*on_interrupt)(void*, void*);
    u64 (*counter_frequency)(void*);
    u64 (*counter_value)(void*);
} ArmExceptions;
typedef struct ArmTraps {
    void* ctx;
    u8 (*is_trapped)(void*, u64);
    u8 (*on_trap)(void*, void*, u64);
    u64 (*on_cache_line)(void*, u64);
} ArmTraps;
typedef struct ArmRegs {
    u64 x[32];
    u8 v[32][16];
    u64 pc;
    u32 nzcv, fpcr, fpsr, pad;
    u64 tpidr, tpidrro;
} ArmRegs;

static struct {
    int on;
    void* lib;
    void* translator;
    void* (*translator_create)(const ArmMemory*);
    void (*translator_invalidate)(void*, u64, u64);
    u8 (*set_host_traps)(const ArmTraps*);
    void* (*context_create)(const ArmExceptions*);
    void (*context_get_regs)(const void*, ArmRegs*);
    void (*context_set_regs)(void*, const ArmRegs*);
    void (*context_request_interrupt)(void*);
    void (*context_resume_running)(void*);
    void (*execute)(void*, void*, u64);
    /* each guest thread's context, by kt_ix, for the kicks: it lives as long as the process */
    void* thread_ctx[KTHREAD_MAX + 1];
} g_armature;

/* this host thread's context (one for each host thread that runs guest code), and the CPU of the innermost run */
static _Thread_local void* t_ctx;
static _Thread_local CPU* t_cpu;
/* the trapped entry that a run of the game's own code there starts at: the trap lets it through once */
static _Thread_local u64 t_bypass;

int armature_on(void) { return g_armature.on; }

/* ---- the registers ---- */

static void to_ctx(void* e, const CPU* c) {
    ArmRegs r;
    memcpy(r.x, c->x, sizeof r.x);
    memcpy(r.v, c->v, sizeof r.v);
    r.pc = c->pc;
    r.nzcv = get_nzcv(c);
    r.fpcr = c->fpcr;
    r.fpsr = c->fpsr;
    r.pad = 0;
    r.tpidr = c->tpidr;
    r.tpidrro = c->tpidrro;
    g_armature.context_set_regs(e, &r);
}

static void from_ctx(const void* e, CPU* c) {
    ArmRegs r;
    g_armature.context_get_regs(e, &r);
    memcpy(c->x, r.x, sizeof r.x);
    memcpy(c->v, r.v, sizeof r.v);
    c->pc = r.pc;
    set_nzcv(c, r.nzcv >> 28);
    c->fpcr = r.fpcr;
    c->fpsr = r.fpsr;
    c->tpidr = r.tpidr;
    c->tpidrro = r.tpidrro;
}

/* ---- memory: the JIT reads and writes guest memory itself (HostMappedUnsafe); these are its fetches of code and
   its slow paths ---- */

static u8 m_read_u8(void* h, u64 a) { (void)h; return *GP(a); }
static u16 m_read_u16(void* h, u64 a) { (void)h; return snx_ld_le16(GP(a)); }
static u32 m_read_u32(void* h, u64 a) { (void)h; return snx_ld_le32(GP(a)); }
static u64 m_read_u64(void* h, u64 a) { (void)h; return snx_ld_le64(GP(a)); }
static void m_read_v128(void* h, u64 a, u8* out) { (void)h; memcpy(out, GP(a), 16); }
static void m_write_u8(void* h, u64 a, u8 v) { (void)h; *GP(a) = v; }
static void m_write_u16(void* h, u64 a, u16 v) { (void)h; snx_st_le16(GP(a), v); }
static void m_write_u32(void* h, u64 a, u32 v) { (void)h; snx_st_le32(GP(a), v); }
static void m_write_u64(void* h, u64 a, u64 v) { (void)h; snx_st_le64(GP(a), v); }
static void m_write_v128(void* h, u64 a, const u8* v) { (void)h; memcpy(GP(a), v, 16); }
static u8 m_is_mapped(void* h, u64 a) { (void)h; (void)a; return 1; }
static void m_signal(void* h, u64 a, u64 size, u8 write) { (void)h; (void)a; (void)size; (void)write; }

/* ---- the callbacks: each copies the registers out, runs the runtime's side on the CPU, and copies them back ---- */

static void after_call(CPU* c) {
    if (__atomic_load_n(&c->poll, __ATOMIC_RELAXED)) c->rt->poll(c);
}

static void on_svc(void* h, void* e, u64 address, int32_t imm) {
    CPU* c = t_cpu;
    u32 n = (u32)imm & 0xFFFFu;
    (void)h;
    from_ctx(e, c);
    c->pc = address + 4;
    if (n < SNX_SVC_COUNT && c->rt->svcs)
        c->rt->svcs[n](c, n);
    else
        c->rt->svc(c, n);
    after_call(c);
    to_ctx(e, c);
}

static void on_brk(void* h, void* e, u64 address, int32_t imm) {
    CPU* c = t_cpu;
    (void)h;
    from_ctx(e, c);
    c->pc = address;
    c->rt->brk(c, address, (u32)imm & 0xFFFFu);
    to_ctx(e, c);
}

static void on_undef(void* h, void* e, u64 address, int32_t word) {
    CPU* c = t_cpu;
    (void)h;
    from_ctx(e, c);
    c->pc = address;
    c->rt->undef(c, address, (u32)word);
    to_ctx(e, c);
}

/* Armature checks for an interrupt with the guest registers in host registers (its CheckSynchronization stores none
   to the context): the poll takes the core and gives it back, and copies no registers */
static void on_interrupt(void* h, void* e) {
    CPU* c = t_cpu;
    (void)h;
    (void)e;
    if (__atomic_load_n(&c->poll, __ATOMIC_RELAXED)) c->rt->poll(c);
}

static u64 counter_frequency(void* h) {
    (void)h;
    return 19200000;
}

static u64 counter_value(void* h) {
    (void)h;
    return guest_ticks();
}

/* ---- the dispatch slots ---- */

static void armature_native(CPU* c, u64 entry);

static GuestFn slot_handler(u64 a) {
    GuestFn f;
    if (a < g_code_lo || a >= g_code_hi || (a & 3) || !g_disp) return NULL;
    f = __atomic_load_n(&g_disp[(a - g_code_lo) >> 2], __ATOMIC_ACQUIRE);
    return f == armature_native ? NULL : f;
}

/* a slot's handler, or the hook that a mod's trampoline of a hooked function takes the first time it runs
   (hook_trampoline, which rt_miss asks for the AOT code: Armature runs all code, so it asks here) */
static GuestFn trap_handler(u64 a) {
    GuestFn f = slot_handler(a);
    return f ? f : hook_trampoline(a);
}

static u8 is_trapped(void* h, u64 a) {
    (void)h;
    return trap_handler(a) ? 1 : 0;
}

/* a block start whose slot a handler holds: the handler runs, and leaves the pc where the guest goes on (the return
   address of the call). The run of the game's own code there (armature_native) passes once */
static u8 on_trap(void* h, void* e, u64 a) {
    CPU* c = t_cpu;
    GuestFn f;
    (void)h;
    if (t_bypass == a) {
        t_bypass = 0;
        return 0;
    }
    f = trap_handler(a);
    if (!f) return 0;
    from_ctx(e, c);
    c->pc = a;
    f(c, a);
    after_call(c);
    to_ctx(e, c);
    return 1;
}

/* ---- runs ---- */

static void* thread_context(void) {
    if (!t_ctx) {
        ArmExceptions x = {NULL, on_brk, on_svc, on_undef, on_interrupt, counter_frequency, counter_value};
        t_ctx = g_armature.context_create(&x);
        if (!t_ctx) fatal("armature: no execution context for a thread");
    }
    return t_ctx;
}

/* c from entry on Armature, until the code returns to 0 */
static void run(CPU* c, u64 entry) {
    void* e = thread_context();
    CPU* outer = t_cpu;
    t_cpu = c;
    c->pc = entry;
    to_ctx(e, c);
    g_armature.context_resume_running(e);
    g_armature.execute(g_armature.translator, e, entry);
    from_ctx(e, c);
    t_cpu = outer;
}

/* the game's own code at entry, as an empty slot means it: a call that returns to the caller (CPU.x[30]) */
static void armature_native(CPU* c, u64 entry) {
    u64 back = c->x[30];
    if (slot_handler(entry)) t_bypass = entry;
    c->x[30] = 0;
    run(c, entry);
    c->x[30] = back;
    c->pc = back;
}

GuestFn armature_slot(void) { return g_armature.on ? armature_native : NULL; }

void armature_slot_taken(u64 addr) {
    if (g_armature.on) g_armature.translator_invalidate(g_armature.translator, addr, 4);
}

void armature_thread_run(struct KThread* t, u64 pc) {
    CPU* c = &t->cpu;
    __atomic_store_n(&g_armature.thread_ctx[kt_ix(t)], thread_context(), __ATOMIC_RELEASE);
    run(c, pc);
}

void armature_kick(struct KThread* t) {
    void* e;
    if (!g_armature.on || !t) return;
    e = __atomic_load_n(&g_armature.thread_ctx[kt_ix(t)], __ATOMIC_ACQUIRE);
    if (e) g_armature.context_request_interrupt(e);
}

/* ---- the library ---- */

static void* sym(const char* name) {
    void* p = plat_dl_sym(g_armature.lib, name);
    if (!p) fatal("armature: %s has no %s: it is older than the runtime's interface (FFI v3)", g_settings.armature_jit, name);
    return p;
}

/* the function pointer in p, as the type of *out (ISO C has no cast between object and function pointers) */
#define SYM(out, name)                                                                                                 \
    do {                                                                                                               \
        void* p__ = sym(name);                                                                                         \
        memcpy(&(out), &p__, sizeof(out));                                                                             \
    } while (0)

void armature_init(void) {
    ArmMemory m = {NULL,          39,           3,           g_mem,        m_read_u8,    m_read_u16,
                   m_read_u32,    m_read_u64,   m_read_v128, m_write_u8,   m_write_u16,  m_write_u32,
                   m_write_u64,   m_write_v128, m_is_mapped, m_signal};
    ArmTraps traps = {NULL, is_trapped, on_trap, NULL};
    if (!g_settings.armature_jit[0]) return;
    if (g_arm_mod) fatal("armature: the game is a 32-bit program, and Armature runs A64 only");
    g_armature.lib = plat_dl_open(g_settings.armature_jit);
    if (!g_armature.lib) fatal("armature: %s could not be loaded: %s", g_settings.armature_jit, plat_dl_error());
    SYM(g_armature.translator_create, "armature_translator_create");
    SYM(g_armature.translator_invalidate, "armature_translator_invalidate");
    SYM(g_armature.set_host_traps, "armature_set_host_traps");
    SYM(g_armature.context_create, "armature_context_create_with_callbacks");
    SYM(g_armature.context_get_regs, "armature_context_get_regs");
    SYM(g_armature.context_set_regs, "armature_context_set_regs");
    SYM(g_armature.context_request_interrupt, "armature_context_request_interrupt");
    SYM(g_armature.context_resume_running, "armature_context_resume_running");
    SYM(g_armature.execute, "armature_execute");
    if (!g_armature.set_host_traps(&traps)) fatal("armature: the library's host traps were set before");
    g_armature.translator = g_armature.translator_create(&m);
    if (!g_armature.translator) fatal("armature: no translator");
    /* the AOT module's functions leave their slots: the game's code is Armature's. The hooks, NVN and the linker
       take theirs after this */
    if (g_disp)
        for (u64 i = 0; i < (g_code_hi - g_code_lo) / 4; i++) __atomic_store_n(&g_disp[i], NULL, __ATOMIC_RELAXED);
    g_armature.on = 1;
    LOG("armature: the game's code runs on %s, not on the recompiled module\n", g_settings.armature_jit);
}
