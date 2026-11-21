/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* armature in the runtime (armature.h): its translator over guest memory, a context per guest thread, the callbacks
   through RtAPI, the kicks for CPU.poll, and the hooks' BRKs with the stubs of their functions' own code */
#include "nx/runtime/armature.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#if !SNX_ARMATURE
void arma_init(void) {
    if (g_settings.armature)
        WARN("armature: armature=%d, but this build has no armature (SNX_ARMATURE off): the recompiled code and the "
             "JIT run the game\n",
             g_settings.armature);
}
int arma_on(void) { return 0; }
void arma_thread_run(struct KThread* t, u64 pc) {
    (void)t;
    (void)pc;
    fatal("armature: a thread ran in armature, which this build does not have");
}
void arma_kick(struct KThread* t) { (void)t; }
GuestFn arma_slot(GuestFn f) { return f; }
void arma_slot_taken(u64 addr) { (void)addr; }
void arma_crash_context(void) {}
void arma_report(void) {}
#else

#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/plat.h"
#include <stdatomic.h>
#include <string.h>
#include <xmmintrin.h>

/* ---- armature's C API (src/ffi.rs, with notas' armature-snx.patch: the CpuState pointer, the stats, a reentrant
   armature_execute and a BRK whose handler picks where the guest goes on) ---- */
typedef struct ArmatureTranslator ArmatureTranslator;
typedef struct ArmatureContext ArmatureContext;
typedef struct {
    void* ctx;
    i32 address_space_bits;
    i32 manager_type; /* 2: HostMapped, guest memory one host block, each address masked to the bits */
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
} ArmatureMemoryCallbacks;
typedef struct {
    void* ctx;
    void (*on_break)(void*, ArmatureContext*, u64, i32);
    void (*on_supervisor_call)(void*, ArmatureContext*, u64, i32);
    void (*on_undefined)(void*, ArmatureContext*, u64, i32);
    void (*on_interrupt)(void*, ArmatureContext*);
    u64 (*counter_frequency)(void*);
    u64 (*counter_value)(void*);
} ArmatureExceptionCallbacks;
typedef struct {
    u64 translated, instructions, with_undefined, lifted, tier1_ns, tier2_ns, map_ns;
} ArmatureStats;
ArmatureTranslator* armature_translator_create(const ArmatureMemoryCallbacks* callbacks);
void armature_translator_invalidate(ArmatureTranslator* translator, u64 address, u64 size);
ArmatureContext* armature_context_create_with_callbacks(const ArmatureExceptionCallbacks* callbacks);
void armature_context_destroy(ArmatureContext* context);
u8* armature_context_native(ArmatureContext* context);
void armature_context_set_x(ArmatureContext* context, i32 index, u64 value);
void armature_context_set_v(ArmatureContext* context, i32 index, const u8* value);
void armature_context_set_pc(ArmatureContext* context, u64 value);
void armature_context_set_nzcv(ArmatureContext* context, u32 value);
void armature_context_set_tpidr(ArmatureContext* context, i64 value);
void armature_context_set_tpidrro(ArmatureContext* context, i64 value);
void armature_context_request_interrupt(ArmatureContext* context);
void armature_context_stop_running(ArmatureContext* context);
void armature_context_resume_running(ArmatureContext* context);
u8 armature_context_is_running(const ArmatureContext* context);
void armature_execute(ArmatureTranslator* translator, ArmatureContext* context, u64 address);
void armature_stats(ArmatureStats* out);

/* armature's CpuState (registers.rs), byte offsets: arma_init checks them through the API's setters */
#define ARMA_ST_X       0u   /* x[32], x[31] the SP */
#define ARMA_ST_V       256u /* v[32], 16 bytes each, lane 0 lowest */
#define ARMA_ST_NZCV    880u /* flags[28]: V, C, Z and N in its bytes 0 to 3; flags[29] to [31] stay 0 */
#define ARMA_ST_FP      896u /* fp_flags[32]: a u32 of 0 or 1 for each bit of FPCR and FPSR */
#define ARMA_ST_TPIDR   1024u
#define ARMA_ST_TPIDRRO 1040u
#define ARMA_ST_PC      1048u       /* dispatch_address: where the guest goes on */
#define ARMA_FPCR_BITS  0x07C09F00u /* the FPCR bits it keeps (registers.rs fpcr::MASK) */
#define ARMA_FPSR_BITS  0x0800009Fu /* and FPSR's (fpsr::MASK) */
#define ARMA_MXCSR_HOST 0x1F80u     /* the runtime's own FP environment: nearest, no flush, every exception masked */

/* the functions handlers take (the hooks', NVN's), each with its stub; the lookup by address, twice as many */
#define ARMA_DIVERTS    8192u
#define ARMA_HASH_BITS  14u
#define ARMA_HASH       (1u << ARMA_HASH_BITS)
#define ARMA_STUB_WORDS 16u
#define ARMA_STUB_BASE  AS_TOP /* past the guest's 39 bits: no guest address, and only armature's code reads see it */
#define ARMA_BRK        (0xD4200000u | (0xA4D7u << 5)) /* what a diverted function's first word reads as */
#define ARMA_SVC_EXIT   0x0Au                          /* svcExitThread */

/* a thread's counts, on a line of their own: by kt_ix, each written by its thread alone */
typedef struct {
    alignas(64) _Atomic u64 svcs;
    _Atomic u64 hooks, originals, interrupts;
} ArmaCounts;

static struct {
    int on;
    ArmatureTranslator* tr;
    /* each thread's context by kt_ix, made on the thread's first run and kept for the slot's later threads, so a
       kick never finds one freed */
    _Atomic(ArmatureContext*) ctx[KTHREAD_MAX + 1];
    _Atomic u8* hooked; /* a bit for each word of [g_code_lo, g_code_hi): a diverted function's first word */
    PMutex lock;        /* the diverts' writers */
    _Atomic u32 diverts;
    u64 entry[ARMA_DIVERTS];
    u32 stub[ARMA_DIVERTS][ARMA_STUB_WORDS];
    _Atomic u64 key[ARMA_HASH]; /* a diverted entry, 0 for none, published after its index */
    u32 index[ARMA_HASH];
    ArmaCounts count[KTHREAD_MAX + 1];
    alignas(64) _Atomic u64 kicks; /* the kernel's, from any thread */
} g_armature = {.lock = PMUTEX_INIT};

/* how deep this thread is in armature: 1 in its own run, more inside a callback's guest calls */
static _Thread_local struct {
    u32 depth;
} t_armature;

int arma_on(void) { return g_armature.on; }

/* ---- guest memory, as armature's code reads see it ---- */

static int hooked(u64 addr) {
    u64 off = addr - g_code_lo, w = off >> 2;
    if (off >= g_code_hi - g_code_lo) return 0;
    return (atomic_load_explicit(&g_armature.hooked[w >> 3], memory_order_acquire) >> (w & 7)) & 1;
}

/* the word armature translates at va: a diverted function's first word reads as the BRK, a stub's words are its
   own, anything outside the guest's addresses UDF #0 */
static u32 code_word(void* user, u64 va) {
    (void)user;
    if (hooked(va)) return ARMA_BRK;
    if (va >= ARMA_STUB_BASE) {
        u64 i = (va - ARMA_STUB_BASE) / (4 * ARMA_STUB_WORDS);
        if (i < atomic_load_explicit(&g_armature.diverts, memory_order_acquire))
            return g_armature.stub[i][(va / 4) % ARMA_STUB_WORDS];
        return 0;
    }
    if (va > g_as_size - 4) return 0;
    return gr32(va);
}
/* armature's host-mapped code reads guest memory itself: these serve the API's other readers, none on this path */
static u8 rd8(void* user, u64 va) {
    (void)user;
    return *GP(va);
}
static u16 rd16(void* user, u64 va) {
    (void)user;
    return snx_ld_le16(GP(va));
}
static u64 rd64(void* user, u64 va) {
    (void)user;
    return snx_ld_le64(GP(va));
}
static void rd128(void* user, u64 va, u8* out) {
    (void)user;
    memcpy(out, GP(va), 16);
}
static void wr8(void* user, u64 va, u8 v) {
    (void)user;
    *GP(va) = v;
}
static void wr16(void* user, u64 va, u16 v) {
    (void)user;
    snx_st_le16(GP(va), v);
}
static void wr32(void* user, u64 va, u32 v) {
    (void)user;
    snx_st_le32(GP(va), v);
}
static void wr64(void* user, u64 va, u64 v) {
    (void)user;
    snx_st_le64(GP(va), v);
}
static void wr128(void* user, u64 va, const u8* v) {
    (void)user;
    memcpy(GP(va), v, 16);
}
static u8 mapped(void* user, u64 va) {
    (void)user;
    return va < g_as_size;
}
static void tracking(void* user, u64 va, u64 size, u8 write) {
    (void)user;
    (void)va;
    (void)size;
    (void)write;
}

/* ---- the state between the CPU and armature's CpuState ---- */

static void state_in(const CPU* c, u8* s) {
    u32 nzcv = c->vf | c->cf << 8 | c->z << 16 | c->n << 24, zero[3] = {0};
    i64 tpidr = (i64)c->tpidr, tpidrro = (i64)c->tpidrro;
    memcpy(s + ARMA_ST_X, c->x, sizeof c->x);
    memcpy(s + ARMA_ST_V, c->v, sizeof c->v);
    memcpy(s + ARMA_ST_NZCV, &nzcv, 4);
    memcpy(s + ARMA_ST_NZCV + 4, zero, sizeof zero);
    for (u32 b = 0; b < 32; b++)
        if ((ARMA_FPCR_BITS | ARMA_FPSR_BITS) >> b & 1) {
            u32 on = ((ARMA_FPCR_BITS >> b & 1 ? c->fpcr : c->fpsr) >> b) & 1;
            memcpy(s + ARMA_ST_FP + 4 * b, &on, 4);
        }
    memcpy(s + ARMA_ST_TPIDR, &tpidr, 8);
    memcpy(s + ARMA_ST_TPIDRRO, &tpidrro, 8);
}
static void state_out(const u8* s, CPU* c) {
    u32 fpcr = c->fpcr & ~ARMA_FPCR_BITS, fpsr = c->fpsr & ~ARMA_FPSR_BITS;
    i64 tpidr, tpidrro;
    memcpy(c->x, s + ARMA_ST_X, sizeof c->x);
    memcpy(c->v, s + ARMA_ST_V, sizeof c->v);
    c->vf = s[ARMA_ST_NZCV] & 1;
    c->cf = s[ARMA_ST_NZCV + 1] & 1;
    c->z = s[ARMA_ST_NZCV + 2] & 1;
    c->n = s[ARMA_ST_NZCV + 3] & 1;
    for (u32 b = 0; b < 32; b++)
        if ((ARMA_FPCR_BITS | ARMA_FPSR_BITS) >> b & 1) {
            u32 on;
            memcpy(&on, s + ARMA_ST_FP + 4 * b, 4);
            if (ARMA_FPCR_BITS >> b & 1)
                fpcr |= (on & 1) << b;
            else
                fpsr |= (on & 1) << b;
        }
    c->fpcr = fpcr;
    c->fpsr = fpsr;
    memcpy(&tpidr, s + ARMA_ST_TPIDR, 8);
    memcpy(&tpidrro, s + ARMA_ST_TPIDRRO, 8);
    c->tpidr = (u64)tpidr;
    c->tpidrro = (u64)tpidrro;
    memcpy(&c->pc, s + ARMA_ST_PC, 8);
}
static void set_pc(u8* s, u64 pc) { memcpy(s + ARMA_ST_PC, &pc, 8); }

/* ---- the diverts: the functions handlers take ---- */

static u32 slot_hash(u64 addr) { return (u32)(((addr >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - ARMA_HASH_BITS)); }

static int divert_of(u64 addr) {
    u32 h = slot_hash(addr);
    for (u32 n = 0; n < ARMA_HASH; n++, h = (h + 1) & (ARMA_HASH - 1)) {
        u64 k = atomic_load_explicit(&g_armature.key[h], memory_order_acquire);
        if (!k) return -1;
        if (k == addr) return (int)g_armature.index[h];
    }
    return -1;
}
static u64 stub_at(int i) { return ARMA_STUB_BASE + (u64)i * 4 * ARMA_STUB_WORDS; }

static u32 movz(u32 d, u64 imm, u32 hw) { return 0xD2800000u | hw << 21 | (u32)(imm & 0xFFFF) << 5 | d; }
static u32 movk(u32 d, u64 imm, u32 hw) { return 0xF2800000u | hw << 21 | (u32)(imm & 0xFFFF) << 5 | d; }
/* Xd = a, a guest address (39 bits) */
static u32 put_address(u32* w, u32 n, u32 d, u64 a) {
    w[n++] = movz(d, a, 0);
    w[n++] = movk(d, a >> 16, 1);
    w[n++] = movk(d, a >> 32, 2);
    return n;
}
static u32 br(u32 n) { return 0xD61F0000u | n << 5; }

/* The stub of the function at entry whose first word is w: w as it runs at entry, then on at entry + 4, through X16
   (X17 where w writes X16), which a call leaves to veneers. Its length in words, 0 for a word it cannot move */
static u32 stub_make(u64 entry, u32 w, u32* out) {
    u32 n = 0, s = (w & 31) == 16 || (w >> 10 & 31) == 16 ? 17 : 16;
    if ((w & 0x1F000000u) == 0x10000000u) { /* ADR, ADRP: the address they make, into Rd */
        u64 imm = (u64)((w >> 29 & 3) | (w >> 5 & 0x7FFFF) << 2), off = (u64)snx_sext64(imm, 21);
        n = put_address(out, n, w & 31, w >> 31 ? (entry & ~0xFFFull) + (off << 12) : entry + off);
    } else if ((w & 0x7C000000u) == 0x14000000u) { /* B, BL: to its target, BL's return address entry + 4 */
        u64 to = entry + ((u64)snx_sext64(w & 0x3FFFFFFu, 26) << 2);
        if (w >> 31) n = put_address(out, n, 30, entry + 4);
        n = put_address(out, n, 16, to);
        out[n++] = br(16);
        return n;
    } else if ((w & 0xFF000000u) == 0x54000000u || (w & 0x7C000000u) == 0x34000000u) {
        /* B.cond, BC.cond, CB(N)Z, TB(N)Z: the branch kept, its target 5 words on, where the stub goes to the guest's */
        int tb = (w & 0x7E000000u) == 0x36000000u;
        u64 field = tb ? (u64)(w >> 5 & 0x3FFF) : (u64)(w >> 5 & 0x7FFFF);
        u64 to = entry + ((u64)snx_sext64(field, tb ? 14 : 19) << 2);
        out[n++] = (w & ~((tb ? 0x3FFFu : 0x7FFFFu) << 5)) | 5u << 5;
        n = put_address(out, n, 16, entry + 4);
        out[n++] = br(16);
        n = put_address(out, n, 16, to);
        out[n++] = br(16);
        return n;
    } else if ((w & 0x3B000000u) == 0x18000000u) { /* LDR (literal), LDRSW, PRFM: the same load from [Xs] */
        static const u32 at[2][4] = {{0xB9400000u, 0xF9400000u, 0xB9800000u, 0xF9800000u},
                                     {0xBD400000u, 0xFD400000u, 0x3DC00000u, 0}};
        u32 v = w >> 26 & 1, opc = w >> 30, t = w & 31;
        u64 from = entry + ((u64)snx_sext64(w >> 5 & 0x7FFFF, 19) << 2);
        if (!at[v][opc]) return 0;
        s = !v && opc < 3 && t == 16 ? 17 : 16;
        n = put_address(out, n, s, from);
        out[n++] = at[v][opc] | s << 5 | t;
    } else {
        if (s == 17 && ((w & 31) == 17 || (w >> 10 & 31) == 17)) return 0;
        out[n++] = w; /* nothing in it reads the pc */
    }
    n = put_address(out, n, s, entry + 4);
    out[n++] = br(s);
    return n;
}

/* The native code an empty slot means, or one that holds this: the function from its own first word, which a divert
   moved to its stub. It returns to 0, which ends the run */
static void arma_native(CPU* c, u64 entry);

GuestFn arma_slot(GuestFn f) { return f || !g_armature.on ? f : arma_native; }

void arma_slot_taken(u64 addr) {
    u32 i, w, h;
    if (!g_armature.on) return;
    if (addr - g_code_lo >= g_code_hi - g_code_lo || (addr & 3))
        fatal("armature: a handler took the slot of %llx, outside the code its dispatch table covers (%llx to %llx)",
              (unsigned long long)addr, (unsigned long long)g_code_lo, (unsigned long long)g_code_hi);
    pm_lock(&g_armature.lock);
    if (divert_of(addr) >= 0) {
        pm_unlock(&g_armature.lock);
        return;
    }
    i = atomic_load_explicit(&g_armature.diverts, memory_order_relaxed); /* under the lock: its only writer */
    if (i == ARMA_DIVERTS) fatal("armature: more than %u functions take handlers", ARMA_DIVERTS);
    w = gr32(addr);
    if (!stub_make(addr, w, g_armature.stub[i]))
        fatal("armature: the function at %llx takes a handler, and its first word %08x is one the prototype cannot "
              "move to a stub",
              (unsigned long long)addr, (unsigned)w);
    g_armature.entry[i] = addr;
    atomic_store_explicit(&g_armature.diverts, i + 1, memory_order_release); /* its stub, before anything reads it */
    for (h = slot_hash(addr); atomic_load_explicit(&g_armature.key[h], memory_order_relaxed);
         h = (h + 1) & (ARMA_HASH - 1)) {}
    g_armature.index[h] = i;
    atomic_store_explicit(&g_armature.key[h], addr, memory_order_release);
    {
        u64 word = (addr - g_code_lo) >> 2;
        atomic_fetch_or_explicit(&g_armature.hooked[word >> 3], (u8)(1u << (word & 7)), memory_order_release);
    }
    pm_unlock(&g_armature.lock);
    /* code armature made before reads the old first word: inlined callees too (cache.rs's get_overlaps) */
    armature_translator_invalidate(g_armature.tr, addr, 4);
}

/* ---- the callbacks ---- */

static void run(CPU* c, ArmatureContext* x, u64 pc);

static u8* native_of(ArmatureContext* x) { return armature_context_native(x); }
static ArmaCounts* counts_of(const CPU* c) { return &g_armature.count[kt_ix((const KThread*)c->thread)]; }

/* SVC #imm, as nce.c's dispatch answers its outcall. svcExitThread in the thread's own run stops it, and thread_run
   then ends the thread outside armature's frames */
static void on_svc(void* host, ArmatureContext* x, u64 addr, i32 imm) {
    CPU* c = (CPU*)host;
    u8* s = native_of(x);
    u32 csr = _mm_getcsr(), n = (u32)imm & 0xFFFFu;
    _mm_setcsr(ARMA_MXCSR_HOST);
    state_out(s, c);
    c->pc = addr + 4;
    atomic_fetch_add_explicit(&counts_of(c)->svcs, 1, memory_order_relaxed);
    if (n == ARMA_SVC_EXIT && t_armature.depth == 1) {
        armature_context_stop_running(x);
        _mm_setcsr(csr);
        return;
    }
    if (n < SNX_SVC_COUNT && c->rt->svcs)
        c->rt->svcs[n](c, n);
    else
        c->rt->svc(c, n);
    if (atomic_load_explicit(&c->poll, memory_order_relaxed)) c->rt->poll(c);
    state_in(c, s);
    _mm_setcsr(csr);
}

/* a BRK: a diverted function's first word runs what its slot holds (the adapter leaves the pc at the function's
   return), or its own code from its stub; any other BRK is the game's, which ends it */
static void on_break(void* host, ArmatureContext* x, u64 addr, i32 imm) {
    CPU* c = (CPU*)host;
    u8* s = native_of(x);
    u32 csr = _mm_getcsr();
    int i = divert_of(addr);
    GuestFn f;
    u64 next;
    _mm_setcsr(ARMA_MXCSR_HOST);
    state_out(s, c);
    if (i < 0) {
        c->pc = addr;
        c->rt->brk(c, addr, (u32)imm & 0xFFFFu);
        fatal("armature: the game's BRK #%x at %llx came back", (unsigned)imm & 0xFFFFu, (unsigned long long)addr);
    }
    f = atomic_load_explicit(&g_disp[(addr - g_code_lo) >> 2], memory_order_acquire);
    if (!f || f == arma_native) {
        atomic_fetch_add_explicit(&counts_of(c)->originals, 1, memory_order_relaxed);
        next = stub_at(i);
    } else {
        atomic_fetch_add_explicit(&counts_of(c)->hooks, 1, memory_order_relaxed);
        c->pc = addr;
        f(c, addr);
        next = c->pc;
    }
    state_in(c, s);
    set_pc(s, next);
    _mm_setcsr(csr);
}

/* an instruction armature has no code for: run again when the guest changed the word since (a mod's trampoline),
   else RtAPI's undef ends the game with its state */
static void on_undefined(void* host, ArmatureContext* x, u64 addr, i32 word) {
    CPU* c = (CPU*)host;
    u8* s = native_of(x);
    u32 csr = _mm_getcsr();
    _mm_setcsr(ARMA_MXCSR_HOST);
    state_out(s, c);
    c->pc = addr;
    if (addr < g_as_size - 3 && !hooked(addr) && gr32(addr) != (u32)word) {
        LOG_LIMIT(16, "armature: code at %llx changed since it was translated (%08x, was %08x): translated again\n",
                  (unsigned long long)addr, gr32(addr), (unsigned)word);
        armature_translator_invalidate(g_armature.tr, addr & ~0xFFFull, 0x1000);
        _mm_setcsr(csr);
        return;
    }
    WARN("armature: no code for instruction %08x at %llx\n", (unsigned)word, (unsigned long long)addr);
    c->rt->undef(c, addr, (u32)word);
    fatal("armature: instruction %08x at %llx came back from the runtime", (unsigned)word, (unsigned long long)addr);
}

/* CPU.poll was set (arma_kick): RtAPI's poll, then again at the next synchronization point while it stays set, as
   generated code calls it at each backward branch */
static void on_interrupt(void* host, ArmatureContext* x) {
    CPU* c = (CPU*)host;
    u8* s = native_of(x);
    u32 csr = _mm_getcsr();
    _mm_setcsr(ARMA_MXCSR_HOST);
    atomic_fetch_add_explicit(&counts_of(c)->interrupts, 1, memory_order_relaxed);
    state_out(s, c);
    if (atomic_load_explicit(&c->poll, memory_order_relaxed)) c->rt->poll(c);
    state_in(c, s);
    if (atomic_load_explicit(&c->poll, memory_order_relaxed)) armature_context_request_interrupt(x);
    _mm_setcsr(csr);
}

static u64 counter_frequency(void* host) {
    (void)host;
    return clock_rate().freq;
}
static u64 counter_value(void* host) {
    (void)host;
    return guest_ticks();
}

/* the context of c's thread, made on its first run (only that thread makes its own) */
static ArmatureContext* context_of(CPU* c) {
    u32 ix = kt_ix((const KThread*)c->thread);
    ArmatureContext* x;
    if (!ix) fatal("armature: guest code ran on a CPU of no guest thread");
    x = atomic_load_explicit(&g_armature.ctx[ix], memory_order_acquire);
    if (!x) {
        ArmatureExceptionCallbacks cb = {.ctx = c,
                                         .on_break = on_break,
                                         .on_supervisor_call = on_svc,
                                         .on_undefined = on_undefined,
                                         .on_interrupt = on_interrupt,
                                         .counter_frequency = counter_frequency,
                                         .counter_value = counter_value};
        x = armature_context_create_with_callbacks(&cb);
        atomic_store_explicit(&g_armature.ctx[ix], x, memory_order_release);
    }
    return x;
}

/* c from pc in armature, the CPU's state in and out around it. Inside a callback (a hook's guest call) the same
   context runs again: the callback took the state out first and puts it back after */
static void run(CPU* c, ArmatureContext* x, u64 pc) {
    u8* s = native_of(x);
    state_in(c, s);
    if (!armature_context_is_running(x)) armature_context_resume_running(x);
    t_armature.depth++;
    armature_execute(g_armature.tr, x, pc);
    t_armature.depth--;
    state_out(s, c);
}

static void arma_native(CPU* c, u64 entry) {
    u64 back = c->x[30], pc = entry;
    int i = divert_of(entry);
    if (i >= 0) pc = stub_at(i);
    c->x[30] = 0; /* the function's return ends the run */
    run(c, context_of(c), pc);
    c->x[30] = back;
    c->pc = back; /* the GuestFn's return: where the caller goes on */
}

void arma_thread_run(struct KThread* t, u64 pc) {
    CPU* c = &t->cpu;
    run(c, context_of(c), pc); /* x30 is 0: the entry function's return ends the thread */
}

void arma_kick(struct KThread* t) {
    ArmatureContext* x;
    if (!g_armature.on || !t) return;
    x = atomic_load_explicit(&g_armature.ctx[kt_ix(t)], memory_order_acquire);
    if (!x) return;
    atomic_fetch_add_explicit(&g_armature.kicks, 1, memory_order_relaxed);
    armature_context_request_interrupt(x);
}

void arma_crash_context(void) {
    KThread* t = g_armature.on ? kthread_cur() : NULL;
    ArmatureContext* x = t ? atomic_load_explicit(&g_armature.ctx[kt_ix(t)], memory_order_acquire) : NULL;
    if (x && t_armature.depth) state_out(native_of(x), &t->cpu);
}

/* mapping, permission and write-back changes (mem_set_code_invalidate): what armature made from the range goes */
static void code_invalidate(u64 addr, u64 size) { armature_translator_invalidate(g_armature.tr, addr, size); }

/* the CpuState offsets this file copies through, against what the API's setters write */
static void layout_check(void) {
    ArmatureExceptionCallbacks none = {0};
    ArmatureContext* x = armature_context_create_with_callbacks(&none);
    u8* s = armature_context_native(x);
    u8 v[16];
    u64 got;
    for (int i = 0; i < 16; i++) v[i] = (u8)(0xA0 + i);
    armature_context_set_x(x, 7, 0x0123456789ABCDEFull);
    armature_context_set_x(x, 31, 0x7766554433221100ull);
    armature_context_set_v(x, 3, v);
    armature_context_set_tpidr(x, 0x1111);
    armature_context_set_tpidrro(x, 0x2222);
    armature_context_set_pc(x, 0x3333);
    armature_context_set_nzcv(x, 0xA0000000u); /* N and C */
    memcpy(&got, s + ARMA_ST_X + 8 * 7, 8);
    if (got != 0x0123456789ABCDEFull)
        fatal("armature: x7 is not at CpuState+%u (%llx there)", ARMA_ST_X + 56, (unsigned long long)got);
    memcpy(&got, s + ARMA_ST_X + 8 * 31, 8);
    if (got != 0x7766554433221100ull)
        fatal("armature: sp is not at CpuState+%u (%llx there)", ARMA_ST_X + 248, (unsigned long long)got);
    if (memcmp(s + ARMA_ST_V + 48, v, 16)) fatal("armature: v3 is not at CpuState+%u", ARMA_ST_V + 48);
    memcpy(&got, s + ARMA_ST_TPIDR, 8);
    if (got != 0x1111)
        fatal("armature: TPIDR_EL0 is not at CpuState+%u (%llx there)", ARMA_ST_TPIDR, (unsigned long long)got);
    memcpy(&got, s + ARMA_ST_TPIDRRO, 8);
    if (got != 0x2222)
        fatal("armature: TPIDRRO_EL0 is not at CpuState+%u (%llx there)", ARMA_ST_TPIDRRO, (unsigned long long)got);
    memcpy(&got, s + ARMA_ST_PC, 8);
    if (got != 0x3333)
        fatal("armature: the pc is not at CpuState+%u (%llx there)", ARMA_ST_PC, (unsigned long long)got);
    if (s[ARMA_ST_NZCV + 3] != 1 || s[ARMA_ST_NZCV + 2] != 0 || s[ARMA_ST_NZCV + 1] != 1 || s[ARMA_ST_NZCV] != 0)
        fatal("armature: NZCV=1010 reads as the bytes %02x %02x %02x %02x at CpuState+%u, not 00 01 00 01",
              s[ARMA_ST_NZCV], s[ARMA_ST_NZCV + 1], s[ARMA_ST_NZCV + 2], s[ARMA_ST_NZCV + 3], ARMA_ST_NZCV);
    armature_context_destroy(x);
}

void arma_init(void) {
    ArmatureMemoryCallbacks mem = {.address_space_bits = 39,
                                   .manager_type = 2,
                                   .page_table_pointer = g_mem,
                                   .read_u8 = rd8,
                                   .read_u16 = rd16,
                                   .read_u32 = code_word,
                                   .read_u64 = rd64,
                                   .read_v128 = rd128,
                                   .write_u8 = wr8,
                                   .write_u16 = wr16,
                                   .write_u32 = wr32,
                                   .write_u64 = wr64,
                                   .write_v128 = wr128,
                                   .is_mapped = mapped,
                                   .signal_memory_tracking = tracking};
    u64 words;
    if (!g_settings.armature) return;
    if (g_arm_mod) {
        WARN("armature: armature=%d, and the game is 32-bit, which armature does not run: the recompiled code "
             "runs it\n",
             g_settings.armature);
        return;
    }
    if (nce_on()) {
        WARN("armature: armature=%d, and the game's code runs natively (NCE): NCE runs it\n", g_settings.armature);
        return;
    }
    if (g_as_size != AS_TOP || mem_identity())
        fatal("armature: armature=%d needs the guest's whole 39-bit address space reserved at g_mem: %llu GiB is "
              "reserved%s",
              g_settings.armature, (unsigned long long)(g_as_size >> 30),
              mem_identity() ? ", at the guest's own addresses" : "");
    words = (g_code_hi - g_code_lo) >> 2;
    g_armature.hooked = (_Atomic u8*)snx_alloc((size_t)(words + 7) / 8);
    memset((void*)g_armature.hooked, 0, (size_t)(words + 7) / 8);
    g_armature.tr = armature_translator_create(&mem);
    layout_check();
    mem_set_code_invalidate(code_invalidate);
    g_armature.on = 1;
    LOG("armature: the game's code runs in armature (guest memory at %p, 39 bits, host-mapped; hooks and NVN by a "
        "BRK at their functions' first words, %llu words of code covered)\n",
        (void*)g_mem, (unsigned long long)words);
}

void arma_report(void) {
    ArmatureStats st;
    u64 svcs = 0, hooks = 0, originals = 0, interrupts = 0;
    if (!g_armature.on) return;
    armature_stats(&st);
    for (u32 i = 0; i <= KTHREAD_MAX; i++) {
        svcs += atomic_load_explicit(&g_armature.count[i].svcs, memory_order_relaxed);
        hooks += atomic_load_explicit(&g_armature.count[i].hooks, memory_order_relaxed);
        originals += atomic_load_explicit(&g_armature.count[i].originals, memory_order_relaxed);
        interrupts += atomic_load_explicit(&g_armature.count[i].interrupts, memory_order_relaxed);
    }
    LOG("armature: %llu functions translated by tier 1 (%llu instructions, %llu with an instruction it has no code "
        "for), %llu by tier 2's SSA compiler; %.3f s in tier 1, %.3f s in tier 2, %.3f s mapping (0 without "
        "ARMATURE_PASS_TIME); %llu SVCs, %llu hook calls, %llu calls of a hooked function's own code, %u diverts, "
        "%llu kicks, %llu interrupts\n",
        (unsigned long long)st.translated, (unsigned long long)st.instructions, (unsigned long long)st.with_undefined,
        (unsigned long long)st.lifted, (double)st.tier1_ns / 1e9, (double)st.tier2_ns / 1e9, (double)st.map_ns / 1e9,
        (unsigned long long)svcs, (unsigned long long)hooks, (unsigned long long)originals,
        atomic_load_explicit(&g_armature.diverts, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_armature.kicks, memory_order_relaxed),
        (unsigned long long)interrupts);
}
#endif
