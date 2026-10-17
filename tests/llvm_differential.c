/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend against the portable one, instruction by instruction. For words of every pattern and case of
   a64_templates.inc, the reference step function (snx_a64_static_step's C, compiled by Clang) and the LLVM
   backend's (snx_ir_static_step, compiled and linked by the backend itself) run from the same random CPU state and
   guest memory; registers, flags, FP state, the exclusive monitor, memory, the returned pc and the runtime callbacks
   must all match.
     llvm_differential CLANG REPO [--match NAME] [--shard I/N] [--per-case N] [--simd N] [--states N] [--seed N]
                       [--quiet] [--jit] [--portable]
   --match keeps patterns whose name starts with NAME. --shard keeps every Nth word from the Ith (from 0): the N runs
   of a seed compare the words one run would. --portable builds the LLVM steps without the host's x86 extensions
   (PSHUFB, PCLMULQDQ, AES-NI, GFNI, VPTERNLOG...), so the emitters' portable forms of those encodings are the ones
   compared. --jit loads the LLVM steps with the JIT's loader (code_loader.c) instead of linking them into a shared
   object for dlopen, and checks that loaded code reaches the runtime's functions and data, outside the code arena. */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/a64.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef SNX_TEST_GCCJIT
#include "common/cpu/recompiler/gccjit/jit.h"
#endif
extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

typedef u64 (*StepFn)(CPU* c);
typedef StepFn* (*TableFn)(void);

#define MEMORY_SIZE 0x220000u
#define PC_BASE     0x100000u
#define DIRECTED    51u /* words directed() makes per round */

static u64 seed = 0x9E3779B97F4A7C15ull;
static u64 next_random(void) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

typedef struct {
    SnxU32 word;
    const SnxA64Pattern* pattern;
    size_t index;
} Word;

/* ---- runtime callbacks, recorded ---- */
typedef struct {
    int kind;
    u64 a, b;
} Event;
typedef struct {
    Event events[16];
    int count;
} Log;
static Log* current;
static void record(int kind, u64 a, u64 b) {
    if (current->count < 16) {
        current->events[current->count].kind = kind;
        current->events[current->count].a = a;
        current->events[current->count++].b = b;
    }
}
static void cb_svc(CPU* c, u32 imm) {
    (void)c;
    record(1, imm, 0);
}
static void cb_undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    record(2, pc, w);
}
static void cb_note(u64 pc) { record(3, pc, 0); }
static u64 cb_ticks(void) {
    record(4, 0, 0);
    return 123456789;
}
static void cb_brk(CPU* c, u64 pc, u32 imm) {
    (void)c;
    record(5, pc, imm);
}
static void cb_fpcr(CPU* c) { record(6, c->fpcr, 0); }
static void cb_sev(void) { record(7, 0, 0); }
static void cb_wfe(CPU* c) {
    (void)c;
    record(8, 0, 0);
}

static int memory_pattern(const char* name) {
    return !strncmp(name, "LDST", 4) || !strcmp(name, "LDR_LIT") || !strncmp(name, "SIMD_LDST", 9) ||
           !strcmp(name, "SYS");
}

static u64 special64(void) {
    static const u64 values[] = {0,
                                 1,
                                 ~0ull,
                                 0x7FFFFFFFFFFFFFFFull,
                                 0x8000000000000000ull,
                                 0xFFFFFFFFull,
                                 0x80000000ull,
                                 0x7FFFFFFFull,
                                 0x3FF0000000000000ull, /* 1.0 */
                                 0x7FF0000000000000ull, /* inf */
                                 0x7FF8000000000001ull, /* qNaN */
                                 0x7FF0000000000001ull, /* sNaN */
                                 0x000FFFFFFFFFFFFFull, /* denormal */
                                 0x3F8000003F800000ull, /* 1.0f, 1.0f */
                                 0x7F8000007FC00001ull, /* inf, qNaN */
                                 0x007FFFFF80000000ull, /* denormal, -0 */
                                 0x7F8000017F800000ull};
    u64 r = next_random();
    if (r % 3 == 0) return values[(r >> 8) % (sizeof(values) / sizeof(values[0]))];
    if (r % 3 == 1) return next_random() >> (r >> 58);
    return next_random();
}

/* floats at the edges of the LLVM backend's FP fast paths: exponents 0, 1, 2, precision+1..3, the top, signed
   zeros, and small integers and products that round or not */
static u32 edge32(void) {
    static const u32 exponents[] = {0, 1, 2, 3, 24, 25, 26, 27, 100, 127, 128, 150, 200, 252, 253, 254, 255};
    u64 r = next_random();
    u32 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u32 m =
        (r >> 8) & 7 ? (u32)(r >> 20) & 0x7FFFFF : (u32)((r >> 20) & 7) << 20; /* sometimes few bits: exact results */
    if (r % 6 == 0) return (u32)((r >> 40) & 1) << 31;
    return (u32)((r >> 40) & 1) << 31 | e << 23 | m;
}
static u64 edge64(void) {
    static const u64 exponents[] = {0, 1, 2, 3, 53, 54, 55, 56, 500, 1023, 1024, 1075, 1500, 2044, 2045, 2046, 2047};
    u64 r = next_random(), r2 = next_random();
    u64 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u64 m = (r >> 8) & 7 ? r2 & 0xFFFFFFFFFFFFFull : (r2 & 7) << 49;
    if (r2 % 6 == 0) return ((r >> 40) & 1) << 63;
    return ((r >> 40) & 1) << 63 | e << 52 | m;
}
static u64 fp_value(void) {
    u64 r = next_random();
    if (r % 4 == 0) return (u64)edge32() << 32 | edge32();
    if (r % 4 == 1) return edge64();
    return special64();
}

/* for SCVTF/UCVTF and FCVT*: two 32-bit lanes or one 64-bit lane of integers near the precision limits of float and
   double (exact up to 2^24 and 2^53, rounded beyond) or small, or of floats near the integer ranges' ends and
   halfway values, a few units in the last place either side, of either sign */
static u64 conversion_edge(void) {
    static const u32 floats[] = {
        0x4B800000u /* 2^24 */, 0x4F000000u /* 2^31 */, 0x4F800000u /* 2^32 */, 0x5F000000u /* 2^63 */,
        0x5F800000u /* 2^64 */, 0x3F000000u /* 0.5 */,  0x3FC00000u /* 1.5 */,  0x40200000u /* 2.5 */};
    static const u64 doubles[] = {0x4340000000000000ull /* 2^53 */, 0x41E0000000000000ull /* 2^31 */,
                                  0x41F0000000000000ull /* 2^32 */, 0x43E0000000000000ull /* 2^63 */,
                                  0x43F0000000000000ull /* 2^64 */, 0x3FE0000000000000ull /* 0.5 */,
                                  0x3FF8000000000000ull /* 1.5 */,  0x4004000000000000ull /* 2.5 */};
    u64 r = next_random(), k = next_random(), lane[2];
    unsigned i;
    if (r % 4 == 1) { /* one 64-bit integer */
        u64 v = (r >> 8) & 1 ? ((u64)1 << 53) + (k % 5) - 2 : k % 4096;
        return (r >> 9) & 1 ? 0 - v : v;
    }
    if (r % 4 == 3) /* one double */
        return ((r >> 9) & 1) << 63 | (doubles[(r >> 10) % 8] + (k % 5) - 2);
    for (i = 0; i < 2; ++i, k >>= 16) {
        u32 v;
        if (r % 4 == 0) { /* 32-bit integers */
            v = (k >> 3) & 1 ? (1u << 24) + (u32)(k % 5) - 2 : (u32)(k >> 4) % 4096;
            if ((k >> 15) & 1) v = 0 - v;
        } else /* floats */
            v = (u32)((k >> 15) & 1) << 31 | (floats[(k >> 4) % 8] + (u32)(k % 5) - 2);
        lane[i] = v;
    }
    return lane[1] << 32 | lane[0];
}

/* for FCVTN and FCVTXN: two doubles or four floats (as the word's sz says) whose exponents, rebiased, are mostly the
   narrower format's smallest and largest normal ones (the fast path's bounds) and now and then just outside them,
   with dropped bits at or around half an ulp, or a fraction of all ones that rounds up into the next exponent */
static u64 narrow_edge(unsigned doubles) {
    static const u32 exponents64[] = {897, 898, 1149, 1150, 897, 898, 1150, 1150, 896, 1151},
                     exponents32[] = {113, 114, 141, 142, 113, 114, 142, 142, 112, 143};
    u64 k = next_random(), lane[2];
    unsigned i, n = doubles ? 1 : 2, shift = doubles ? 29 : 13, mantissa = doubles ? 52 : 23;
    for (i = 0; i < n; ++i, k >>= 16) {
        u64 low = (u64)1 << (shift - 1), fraction;
        switch (k % 6) {
            case 0: fraction = 0; break;
            case 1: fraction = low; break;
            case 2: fraction = low + 1; break;
            case 3: fraction = low - 1; break;
            case 4: fraction = ((u64)1 << mantissa) - 1; break;
            default: fraction = next_random() & (((u64)1 << mantissa) - 1); break;
        }
        if ((k >> 3) % 2 && k % 6 != 4) fraction ^= (next_random() << shift) & (((u64)1 << mantissa) - 1);
        lane[i] = ((k >> 4) & 1) << (doubles ? 63 : 31) |
                  (u64)(doubles ? exponents64[(k >> 5) % 10] : exponents32[(k >> 5) % 10]) << mantissa | fraction;
    }
    return doubles ? lane[0] : lane[1] << 32 | lane[0];
}

static void random_state(CPU* c, unsigned char* memory, const Word* w) {
    unsigned i;
    int addresses = memory_pattern(w->pattern->name);
    static const u32 fpcr[] = {0, 1u << 24, 1u << 25, 3u << 22, 1u << 22, 2u << 22, 1u << 26, 0x07C00000u};
    for (i = 0; i < 32; ++i) {
        c->x[i] = addresses ? 0x1000 + (next_random() % 0x7000 & ~(u64)15) : special64();
        c->v[i].d[0] = fp_value();
        c->v[i].d[1] = fp_value();
    }
    {
        /* the instruction's own FP operands (Rn, Rm, Ra) now and then: Rm or Ra a copy of Rn or its negation lane
           by lane, for sums that cancel exactly; a register of zeros of random signs, so every lane's product has a
           zero operand, or two, for FMAX/FMIN's +0/-0 ties; and a vector whose lanes need different checks: 1*1 is exact, and 0x3F800001 * 0x01800001
           is inexact with an FMA residual that underflows to zero */
        static const u64 negations[] = {0, 0x8000000080000000ull, 0x8000000000000000ull};
        unsigned rn = (w->word >> 5) & 31, rm = (w->word >> 16) & 31, ra = (w->word >> 10) & 31;
        u64 r = next_random(), negate = negations[(r >> 8) % 3];
        if (r % 4 == 0) {
            c->v[rm].d[0] = c->v[rn].d[0] ^ negate;
            c->v[rm].d[1] = c->v[rn].d[1] ^ negate;
        }
        if ((r >> 16) % 8 == 0) {
            c->v[ra].d[0] = c->v[rn].d[0] ^ negate;
            c->v[ra].d[1] = c->v[rn].d[1] ^ negate;
        }
        if ((r >> 24) % 12 == 0) {
            V128* zero = &c->v[(r >> 32) % 2 ? rm : rn];
            zero->d[0] = next_random() & 0x8000000080000000ull;
            zero->d[1] = next_random() & 0x8000000080000000ull;
            if ((r >> 34) % 2) {
                zero = &c->v[(r >> 32) % 2 ? rn : rm];
                zero->d[0] = next_random() & 0x8000000080000000ull;
                zero->d[1] = next_random() & 0x8000000080000000ull;
            }
        }
        if ((r >> 40) % 24 == 0) {
            c->v[rn].d[0] = c->v[rn].d[1] = 0x3F8000013F800000ull;
            c->v[rm].d[0] = c->v[rm].d[1] = 0x018000013F800000ull;
        }
        if ((r >> 48) % 4 == 0) {
            c->v[rn].d[0] = conversion_edge();
            c->v[rn].d[1] = conversion_edge();
        }
        if ((w->word & 0x8F3FFC00UL) == 0x0E216800UL && (r >> 52) % 4) { /* two-register misc 0x16: FCVTN, FCVTXN */
            c->v[rn].d[0] = narrow_edge((w->word >> 22) & 1);
            c->v[rn].d[1] = narrow_edge((w->word >> 22) & 1);
        }
    }
    c->n = next_random() & 1;
    c->z = next_random() & 1;
    c->cf = next_random() & 1;
    c->vf = next_random() & 1;
    c->fpcr = fpcr[next_random() % 8] | (next_random() % 2 ? (u32)(next_random() & 0x07C00000u) : 0);
    if ((w->word & 0x8F3FFC00UL) == 0x0E216800UL && next_random() % 2) c->fpcr &= ~(3u << 22); /* FCVTN: RNE */
    c->fpsr = next_random() % 4 ? (next_random() % 2 ? 0x10 : 0) : (u32)(next_random() & 0xF800009Fu);
    c->tpidr = next_random();
    c->tpidrro = next_random();
    c->excl_addr = next_random();
    c->excl_val = next_random();
    c->excl_val2 = next_random();
    c->excl_size = (u32)(next_random() % 4);
    c->excl_on = next_random() & 1;
    if (!strcmp(w->pattern->name, "LDSTX") && next_random() % 2) {
        /* a monitor that matches, holding memory's current value, so exclusive stores can succeed */
        unsigned rn = (w->word >> 5) & 31;
        u64 a = c->x[rn];
        c->excl_on = 1;
        c->excl_addr = a;
        c->excl_size = ((w->word >> 30) & 3) | ((w->word >> 21) & 1 ? 0x10 : 0);
        memcpy(&c->excl_val, memory + a, 8);
        if (!((w->word >> 21) & 1) && (w->word >> 30) < 3) c->excl_val &= (1ull << (8u << (w->word >> 30))) - 1;
        if ((w->word >> 21) & 1 && (w->word >> 30) == 2)
            c->excl_val2 = 0;
        else
            memcpy(&c->excl_val2, memory + a + 8, 8);
    }
}

static int same_events(const Log* a, const Log* b) {
    int i;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; ++i)
        if (a->events[i].kind != b->events[i].kind || a->events[i].a != b->events[i].a ||
            a->events[i].b != b->events[i].b)
            return 0;
    return 1;
}

static void report(const Word* w, const CPU* start, const CPU* p, const CPU* l, u64 rp, u64 rl, const Log* lp,
                   const Log* ll, int memory_differs) {
    unsigned i;
    fprintf(stderr, "MISMATCH %s case %lu word %08lX\n", w->pattern->name, (unsigned long)w->index,
            (unsigned long)w->word);
    if (rp != rl)
        fprintf(stderr, "  returned pc %llx (portable) %llx (llvm)\n", (unsigned long long)rp, (unsigned long long)rl);
    for (i = 0; i < 32; ++i)
        if (p->x[i] != l->x[i])
            fprintf(stderr, "  x%u: start %016llx portable %016llx llvm %016llx\n", i, (unsigned long long)start->x[i],
                    (unsigned long long)p->x[i], (unsigned long long)l->x[i]);
    for (i = 0; i < 32; ++i)
        if (memcmp(&p->v[i], &l->v[i], 16))
            fprintf(stderr, "  v%u: start %016llx:%016llx portable %016llx:%016llx llvm %016llx:%016llx\n", i,
                    (unsigned long long)start->v[i].d[1], (unsigned long long)start->v[i].d[0],
                    (unsigned long long)p->v[i].d[1], (unsigned long long)p->v[i].d[0],
                    (unsigned long long)l->v[i].d[1], (unsigned long long)l->v[i].d[0]);
    if (p->n != l->n || p->z != l->z || p->cf != l->cf || p->vf != l->vf)
        fprintf(stderr, "  nzcv portable %u%u%u%u llvm %u%u%u%u\n", p->n, p->z, p->cf, p->vf, l->n, l->z, l->cf, l->vf);
    if (p->fpcr != l->fpcr || p->fpsr != l->fpsr)
        fprintf(stderr, "  fpcr/fpsr portable %08x/%08x llvm %08x/%08x (start %08x/%08x)\n", p->fpcr, p->fpsr, l->fpcr,
                l->fpsr, start->fpcr, start->fpsr);
    if (p->pc != l->pc)
        fprintf(stderr, "  c->pc portable %llx llvm %llx\n", (unsigned long long)p->pc, (unsigned long long)l->pc);
    if (p->excl_on != l->excl_on || p->excl_addr != l->excl_addr || p->excl_val != l->excl_val ||
        p->excl_val2 != l->excl_val2 || p->excl_size != l->excl_size)
        fprintf(stderr, "  exclusive monitor differs\n");
    if (p->tpidr != l->tpidr) fprintf(stderr, "  tpidr differs\n");
    if (memory_differs) fprintf(stderr, "  guest memory differs\n");
    if (!same_events(lp, ll)) fprintf(stderr, "  runtime callbacks differ (%d vs %d)\n", lp->count, ll->count);
}

static int same_cpu(const CPU* a, const CPU* b) {
    return !memcmp(a->x, b->x, sizeof(a->x)) && a->pc == b->pc && a->n == b->n && a->z == b->z && a->cf == b->cf &&
           a->vf == b->vf && !memcmp(a->v, b->v, sizeof(a->v)) && a->fpcr == b->fpcr && a->fpsr == b->fpsr &&
           a->tpidr == b->tpidr && a->tpidrro == b->tpidrro && a->excl_addr == b->excl_addr &&
           a->excl_val == b->excl_val && a->excl_val2 == b->excl_val2 && a->excl_size == b->excl_size &&
           a->excl_on == b->excl_on;
}

static void* load(const char* path, TableFn* table) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    *table = h ? (TableFn)(void*)GetProcAddress(h, "snx_test_steps") : NULL;
    return h;
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "%s\n", dlerror());
    *table = h ? (TableFn)dlsym(h, "snx_test_steps") : NULL;
    return h;
#endif
}

/* --jit: a unit that reaches into this executable, as JIT code reaches into the runtime: a hidden function, called
   (R_X86_64_PLT32, through a stub when it is out of reach), and a default-visibility variable (through the GOT) */
static u64 probe_value = 0x1234;
static u64 probe_add(u64 a) { return a + 7; }

static int load_probe(SnxCodeLoader* loader) {
    char error[1024];
    const char* names[1] = {"snx_test_probe"};
    SnxU64 address;
    SnxIr* ir = snx_ir_new("probe", '2', error, sizeof(error));
    LLVMValueRef value, add, probe, args[1];
    LLVMTypeRef params[1];
    unsigned char* object;
    size_t object_size, used;
    SnxLinkInput input;
    u64 (*call)(u64);
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, "snx_test_probe_value");
    add = ir_extern(ir, "snx_test_probe_add", ir->t_i64, params, 1);
    probe = LLVMAddFunction(ir->mod, "snx_test_probe", LLVMFunctionType(ir->t_i64, params, 1, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, probe, "entry"));
    args[0] = LLVMGetParam(probe, 0);
    LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, value, "v"),
                                     LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(add), add, args, 1, "c"), "r"));
    if (snx_ir_object(ir, '2', &object, &object_size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    input.data = object;
    input.size = object_size;
    input.name = "probe";
    if (snx_code_define(loader, "snx_test_probe_value", (SnxU64)(size_t)&probe_value) ||
        snx_code_define(loader, "snx_test_probe_add", (SnxU64)(size_t)&probe_add))
        return snx_error("jit: the probe's symbols are defined already");
    if (snx_code_load(loader, &input, 1, names, 1, &address, error, sizeof(error))) return snx_error("jit: %s", error);
    call = (u64 (*)(u64))(size_t)address;
    if (call(100) != 100 + 7 + 0x1234) return snx_error("jit: the probe returned %llx", (unsigned long long)call(100));
    /* the same unit again defines snx_test_probe twice: refused, with the arena as it was */
    used = snx_code_used(loader);
    if (!snx_code_load(loader, &input, 1, NULL, 0, NULL, error, sizeof(error)) || snx_code_used(loader) != used)
        return snx_error("jit: a second snx_test_probe was loaded");
    free(object);
    /* an empty unit names what earlier ones defined */
    if (snx_code_load(loader, NULL, 0, names, 1, &address, error, sizeof(error)) || address != (SnxU64)(size_t)call)
        return snx_error("jit: snx_test_probe is not where it was loaded");
    return 0;
}

/* words of one case: random operand fields, the case's specialization fields set, valid encodings only */
static size_t generate(const SnxA64Pattern* p, size_t index, size_t want, Word* out) {
    size_t got = 0, attempts;
    for (attempts = 0; attempts < want * 400 && got < want; ++attempts) {
        SnxU32 w = ((SnxU32)next_random() & ~p->mask) | p->value;
        size_t k = index;
        int n;
        for (n = (int)p->field_count - 1; n >= 0; --n) {
            SnxU32 mask = (SnxU32)((1UL << p->fields[n].width) - 1);
            w = (w & ~(mask << p->fields[n].lo)) | ((SnxU32)(k & mask) << p->fields[n].lo);
            k >>= p->fields[n].width;
        }
        if (snx_a64_decode(w) != &p->cases[index]) continue;
        out[got].word = w;
        out[got].pattern = p;
        out[got++].index = index;
    }
    return got;
}

/* snx_a64_fast_simd's encodings, which random fields rarely produce: 3-same logical/FP/ADD, FP by element, DUP/INS/
   UMOV, MOVI #0 and MOV (ORR Vd,Vn,Vn), with random operands */
static size_t directed(const char* match, size_t want, Word* out) {
    static const SnxU32 same_ops[] = {3, 0x10, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
                        element_ops[] = {1, 5, 9}, copy_ops[] = {0, 1, 3, 7}, fp_ops[] = {0x18, 0x1A, 0x1C, 0x1D, 0x1E};
    const size_t nsame = sizeof(same_ops) / sizeof(same_ops[0]);
    size_t got = 0, i;
    if (match && strncmp("SIMD", match, strlen(match) < 4 ? strlen(match) : 4)) return 0;
    for (i = 0; i < want; ++i) {
        SnxU32 r = (SnxU32)next_random(), n = (r >> 5) & 31, words[DIRECTED];
        unsigned k;
        words[0] = 0x2F00E400UL | (r & 31) | ((r >> 9) & 1) << 30;
        words[1] = 0x0EA01C00UL | ((r >> 10) & 1) << 30 | n << 16 | n << 5 | (r & 31);
        words[2] = ((r & ~0x9F20FC00UL) | 0x0E200400UL) | same_ops[next_random() % nsame] << 11;
        words[3] = ((r & ~0x9F00F400UL) | (next_random() % 2 ? 0x5F000000UL : 0x0F000000UL) | 0x00800000UL) |
                   element_ops[next_random() % 3] << 12;
        words[4] = ((r & ~0x9FE0FC00UL) | 0x0E000400UL) | copy_ops[next_random() % 4] << 11;
        words[5] = ((r & ~0x9F20FC00UL) | 0x0E200400UL) | same_ops[next_random() % nsame] << 11 | 0x40000000UL;
        /* snx_ir_more_simd: LD1/ST1 (1-4 registers), LD1R and lanes, CNT/NOT/RBIT, PMULL 1Q, MOVI family, UZP/TRN/ZIP,
           EXT, AES */
        words[6] = (r & 0x40C0001FUL) | 0x0C000000UL | (SnxU32)(next_random() % 2) << 23 |
                   (SnxU32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16 |
                   (SnxU32) "\x07\x0A\x06\x02"[next_random() % 4] << 12 | ((r >> 10) & 3) << 10 | ((r >> 5) & 31) << 5;
        words[7] = (r & 0x40E0FFFFUL & ~0x00202000UL) | 0x0D000000UL;
        words[8] = (r & 0x60C003FFUL) | 0x0E205800UL;
        words[9] = (r & 0x401F03FFUL) | 0x0EE0E000UL;
        words[10] = (r & 0x6007FBFFUL) | 0x0F000400UL;
        words[11] = (r & 0x40DF73FFUL) | 0x0E000800UL;
        words[12] = (r & 0x401F7BFFUL) | 0x2E000000UL;
        words[13] = (r & 0x000033FFUL) | 0x4E284800UL;
        /* three-same integer, shifts by immediate, FABS/FNEG */
        words[14] = (r & 0x60DFFBFFUL) | 0x0E200400UL;
        words[15] = (r & 0x607FFBFFUL & ~0x0000F800UL) | 0x0F000400UL |
                    (SnxU32) "\x00\x02\x08\x0A\x14\x10"[next_random() % 6] << 11 | (1u << (19 + (r >> 30) % 4));
        words[16] = (r & 0x60C003FFUL) | 0x0EA0F800UL;
        /* saturating and halving adds, BSL/BIT/BIF: three-same ops 0x00-0x05 and 3 with U */
        words[17] =
            (r & 0x40DF07FFUL) | 0x0E200400UL | (SnxU32)(next_random() % 6) << 11 | (SnxU32)(next_random() % 2) << 29;
        words[18] = (r & 0x40DF03FFUL) | 0x2E201C00UL | (1u + (SnxU32)(next_random() % 3)) << 22;
        words[19] = (r & 0x40DF03FFUL) | 0x2E605800UL; /* RBIT */
        /* FP three-same beyond fast_simd (FADDP, FMAX/FMIN and NM, pairwise, FCMEQ/GE/GT, FACGE/GT) and the FP
           compares against #0.0 (2-reg misc 0x0C-0x0E), with random Q, U and sz */
        words[20] = (r & 0x60DF03FFUL) | 0x0E200400UL | fp_ops[next_random() % 5] << 11;
        words[21] = (r & 0x604003FFUL) | 0x0EA00800UL | (0x0Cu + (SnxU32)(next_random() % 3)) << 12;
        /* FP two-register misc, vector and scalar: FCVT[NPMZA][SU], SCVTF/UCVTF, FRECPE/FRSQRTE, FSQRT (k = U:o2:op);
           three-same FABD, FMULX, FRECPS, FRSQRTS (U:o2:op), vector and scalar; SCVTF/UCVTF/FCVTZS/FCVTZU fixed
           point (shift by immediate, 32 or 64-bit lanes), vector and scalar */
        {
            static const SnxU32 misc[] = {0x1A, 0x1B, 0x1C, 0x1D, 0x3A, 0x3B, 0x3D, 0x5A,
                                          0x5B, 0x5C, 0x5D, 0x7A, 0x7B, 0x7D, 0x7F},
                                steps[] = {0x7A, 0x1B, 0x1F, 0x3F};
            SnxU32 m = misc[next_random() % 15], t = steps[next_random() % 4], f = next_random() % 2 ? 0x1C : 0x1F;
            m = (m >> 6) << 29 | ((m >> 5) & 1) << 23 | (m & 31) << 12;
            t = (t >> 6) << 29 | ((t >> 5) & 1) << 23 | (t & 31) << 11;
            words[22] = (r & 0x404003FFUL) | 0x0E200800UL | m;
            words[23] = (r & 0x004003FFUL) | 0x5E200800UL | m;
            words[24] = (r & 0x405F03FFUL) | 0x0E200400UL | t;
            words[25] = (r & 0x005F03FFUL) | 0x5E200400UL | t;
            words[26] = (r & 0x607F03FFUL) | 0x0F200400UL | f << 11;
            words[27] = (r & 0x207F03FFUL) | 0x5F200400UL | f << 11;
        }
        /* the integer and data-movement rest: scalar DUP, SMOV, TBL/TBX (1-4 registers), LD2-4/ST2-4 (multiple
           structures), every single-structure form, three-different, integer two-register misc, across lanes, scalar
           pairwise, by element (vector and scalar), the shifts by immediate beyond words[15] (vector and scalar), the
           register shifts, SHSUB and SQDMULH/SQRDMULH, the scalar three-same integer ops, the scalar integer
           two-register misc and three-different ops, and PMUL/PMULL on bytes */
        {
            static const unsigned char misc[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x07,
                                                 0x08, 0x09, 0x0A, 0x0B, 0x12, 0x13, 0x14},
                                       across[] = {0x03, 0x0A, 0x1A, 0x1B, 0x0C, 0x0F},
                                       pairwise[] = {0x1B, 0x0D, 0x0C, 0x0F},
                                       element[] = {0, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13},
                                       shifts[] = {0x04, 0x06, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13},
                                       scalar_shifts[] = {0x00, 0x02, 0x04, 0x06, 0x08, 0x0A,
                                                          0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13},
                                       scalar_misc[] = {0x03, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x12, 0x14},
                                       same[] = {0x04, 0x08, 0x09, 0x0A, 0x0B, 0x16},
                                       scalar_same[] = {0x01, 0x05, 0x06, 0x07, 0x08, 0x09,
                                                        0x0A, 0x0B, 0x10, 0x11, 0x16};
            SnxU32 immh = 1u << (19 + (SnxU32)(next_random() % 4));
            words[28] = (r & 0x001F03FFUL) | 0x5E000400UL;
            words[29] = (r & 0x401F03FFUL) | 0x0E002C00UL;
            words[30] = (r & 0x401F73FFUL) | 0x0E000000UL;
            words[31] = (r & 0x40C0001FUL) | 0x0C000000UL | (SnxU32)(next_random() % 2) << 23 |
                        (SnxU32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16 |
                        (SnxU32) "\x00\x04\x08"[next_random() % 3] << 12 | ((r >> 10) & 3) << 10 | ((r >> 5) & 31) << 5;
            words[32] = (r & 0x40FFFFFFUL & ~0x001F0000UL) | 0x0D000000UL |
                        (SnxU32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16;
            words[33] = (r & 0x60DFF3FFUL) | 0x0E200000UL;
            words[34] = (r & 0x60C003FFUL) | 0x0E200800UL | (SnxU32)misc[next_random() % sizeof(misc)] << 12;
            words[35] = (r & 0x60C003FFUL) | 0x0E300800UL | (SnxU32)across[next_random() % sizeof(across)] << 12;
            words[36] = (r & 0x20C003FFUL) | 0x5E300800UL | (SnxU32)pairwise[next_random() % sizeof(pairwise)] << 12;
            words[37] = (r & 0x60FF0BFFUL) | 0x0F000000UL | (SnxU32)element[next_random() % sizeof(element)] << 12 |
                        (next_random() % 2 ? 0x50000000UL : 0);
            words[38] = (r & 0x6007FBFFUL & ~0x0000F800UL) | 0x0F000400UL |
                        (SnxU32)shifts[next_random() % sizeof(shifts)] << 11 | immh;
            words[39] = (r & 0x2007FBFFUL & ~0x0000F800UL) | 0x5F000400UL |
                        (SnxU32)scalar_shifts[next_random() % sizeof(scalar_shifts)] << 11 |
                        (next_random() % 2 ? 1u << 22 : immh);
            words[40] = (r & 0x60DF07FFUL) | 0x0E200400UL | (SnxU32)same[next_random() % sizeof(same)] << 11;
            words[41] =
                (r & 0x20DF07FFUL) | 0x5E200400UL | (SnxU32)scalar_same[next_random() % sizeof(scalar_same)] << 11;
            words[42] =
                (r & 0x20C003FFUL) | 0x5E200800UL | (SnxU32)scalar_misc[next_random() % sizeof(scalar_misc)] << 12;
            words[43] = (r & 0x00DF03FFUL) | 0x5E200000UL | (SnxU32) "\x09\x0B\x0D"[next_random() % 3] << 12;
            words[44] = (r & 0x401F03FFUL) | (next_random() % 2 ? 0x2E209C00UL : 0x0E20E000UL); /* PMUL, PMULL 8B */
        }
        /* the scalar FP compares (FCMEQ, FCMGE, FCMGT, FACGE, FACGT and against #0.0), FCVTN/FCVTXN (vector, with Q
           the "2" forms, and scalar), SHA1C/P/M, SHA1SU0, SHA256H/H2, SHA256SU1 (three registers, op 7 unallocated)
           and SHA1H, SHA1SU1, SHA256SU0 (two, op 3 unallocated), with random U, sz and o2 */
        words[45] = (r & 0x20DF03FFUL) | 0x5E200400UL | (0x1Cu + (SnxU32)(next_random() % 2)) << 11;
        words[46] = (r & 0x204003FFUL) | 0x5EA00800UL | (0x0Cu + (SnxU32)(next_random() % 3)) << 12;
        words[47] = (r & 0x604003FFUL) | 0x0E216800UL;
        words[48] = (r & 0x204003FFUL) | 0x5E216800UL | (next_random() % 4 ? 0x20400000UL : 0);
        words[49] = (r & 0x001F03FFUL) | 0x5E000000UL | (SnxU32)(next_random() % 8) << 12;
        words[50] = (r & 0x000003FFUL) | 0x5E280800UL | (SnxU32)(next_random() % 4) << 12;
        for (k = 0; k < DIRECTED; ++k) {
            const SnxA64Case* c = snx_a64_decode(words[k]);
            if (c->kind != SNX_FLOW_PLAIN) continue;
            out[got].word = words[k];
            out[got].pattern = snx_a64_case_pattern(c, &out[got].index);
            if (out[got].pattern) ++got;
        }
    }
    return got;
}

#ifdef SNX_TEST_GCCJIT
/* the libgccjit backend's step functions (snx_jit_static_step), linked by GCC's driver with the runtime library */
static int build_gccjit(const Word* words, size_t count, const char* dir, const char* path, unsigned long* fallbacks) {
    SnxJit* jit = snx_jit_new('2');
    gcc_jit_rvalue** fns = (gcc_jit_rvalue**)snx_alloc(count * sizeof(*fns));
    gcc_jit_type *param = jit->t_ptr, *step_ptr, *table_type;
    gcc_jit_lvalue* table;
    gcc_jit_function* getter;
    char* rtlib = snx_path_join(dir, "rtlib.o");
    size_t i;
    int rc = 0;
    step_ptr = gcc_jit_context_new_function_ptr_type(jit->ctx, NULL, jit->t_u64, 1, &param, 0);
    for (i = 0; i < count; ++i) {
        char name[32];
        sprintf(name, "S_%lu", (unsigned long)i);
        fns[i] = gcc_jit_context_new_cast(
            jit->ctx, NULL, gcc_jit_function_get_address(snx_jit_static_step(jit, words[i].word, PC_BASE, name), NULL),
            step_ptr);
    }
    table_type = gcc_jit_context_new_array_type(jit->ctx, NULL, step_ptr, (int)count);
    table = gcc_jit_context_new_global(jit->ctx, NULL, GCC_JIT_GLOBAL_INTERNAL, table_type, "TABLE");
    gcc_jit_global_set_initializer_rvalue(
        table, gcc_jit_context_new_array_constructor(jit->ctx, NULL, table_type, count, fns));
    getter = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_ptr, "snx_test_steps", 0,
                                          NULL, 0);
    gcc_jit_block_end_with_return(
        gcc_jit_function_new_block(getter, "entry"), NULL,
        gcc_jit_context_new_cast(jit->ctx, NULL, gcc_jit_lvalue_get_address(table, NULL), jit->t_ptr));
    snx_write_file(rtlib, snx_rtlib_object, snx_rtlib_object_size);
    gcc_jit_context_add_driver_option(jit->ctx, rtlib);
    gcc_jit_context_compile_to_file(jit->ctx, GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY, path);
    if (snx_jit_error(jit)) rc = snx_error("libgccjit: %s", snx_jit_error(jit));
    *fallbacks = jit->fallbacks;
    snx_jit_free(jit);
    free(fns);
    free(rtlib);
    return rc;
}
#endif

int main(int argc, char** argv) {
    const char *clang, *repo, *match = NULL;
    size_t per_case = 4, per_simd = 300, states = 6, count = 0, capacity = 0, npatterns, i, k, failures = 0,
           compared = 0, shard = 0, shards = 1;
    size_t pattern_failures[64] = {0};
    Word* words = NULL;
    const SnxA64Pattern* patterns = snx_a64_patterns(&npatterns);
    char error[1024], *dir, *c_path, *c_lib, *l_lib, *text = NULL, *abi, *root;
    const char* args[24];
    FILE* f;
    SnxIr* ir;
    unsigned char *object = NULL, *memory_start, *memory_p, *memory_l;
    size_t object_size;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    const char* exports[1] = {"snx_test_steps"};
    TableFn table_p, table_l;
    StepFn *steps_p, *steps_l;
    RtAPI rt;
    int quiet = 0, a, gccjit = 0, jit = 0, portable = 0, march;
    SnxCodeLoader* loader = NULL;
#ifdef SNX_TEST_GCCJIT
    TableFn table_g = NULL;
#endif
    StepFn* steps_g = NULL;
    TableFn table_h;
    StepFn* steps_h;
    char* c_native;
    unsigned long fallbacks, gccjit_fallbacks = 0;
    if (argc < 3) {
        fputs("usage: llvm_differential CLANG REPO [--match NAME] [--shard I/N] [--per-case N] [--simd N] [--states N]"
              " [--seed N] [--quiet] [--gccjit] [--jit] [--portable]\n",
              stderr);
        return 2;
    }
    clang = argv[1];
    repo = argv[2];
    for (a = 3; a < argc; ++a) {
        if (!strcmp(argv[a], "--quiet"))
            quiet = 1;
        else if (!strcmp(argv[a], "--gccjit"))
            gccjit = 1;
        else if (!strcmp(argv[a], "--jit"))
            jit = 1;
        else if (!strcmp(argv[a], "--portable"))
            portable = 1;
        else if (a + 1 < argc && !strcmp(argv[a], "--match"))
            match = argv[++a];
        else if (a + 1 < argc && !strcmp(argv[a], "--shard")) {
            char* end;
            shard = strtoul(argv[++a], &end, 10);
            shards = *end == '/' ? strtoul(end + 1, &end, 10) : 0;
            if (*end || shard >= shards) return snx_error("--shard wants I/N with I below N, not %s", argv[a]);
        } else if (a + 1 < argc && !strcmp(argv[a], "--per-case"))
            per_case = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--simd"))
            per_simd = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--states"))
            states = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--seed"))
            seed = strtoull(argv[++a], NULL, 0) | 1;
        else
            return snx_error("unknown argument %s", argv[a]);
    }
    /* words */
    for (i = 0; i < npatterns; ++i) {
        const SnxA64Pattern* p = &patterns[i];
        if (match && strncmp(p->name, match, strlen(match))) continue;
        for (k = 0; k < p->case_count; ++k) {
            size_t want = !strncmp(p->name, "SIMD", 4) ? per_simd : per_case;
            if (p->cases[k].kind == SNX_FLOW_UNDEF) continue;
            if (count + want > capacity) {
                capacity = (count + want) * 2;
                words = (Word*)snx_realloc(words, capacity * sizeof(*words));
            }
            count += generate(p, k, want, words + count);
        }
    }
    words = (Word*)snx_realloc(words, (count + DIRECTED * per_simd) * sizeof(*words));
    count += directed(match, per_simd, words + count);
    for (i = shard, k = 0; i < count; i += shards) words[k++] = words[i];
    count = k;
    if (!count) return snx_error("no words to test");
    dir = snx_temp_dir(NULL);
    c_path = snx_path_join(dir, "portable.c");
    c_lib = snx_path_join(dir, "portable.so");
    l_lib = snx_path_join(dir, "llvm.so");
    /* the reference: snx_a64_static_step's C for each word, compiled by Clang with the flags generated C had */
    f = snx_fopen(c_path, "wb");
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\ntypedef u64 (*StepFnPtr)(CPU*);\n", f);
    for (i = 0; i < count; ++i) {
        char name[32];
        sprintf(name, "S_%lu", (unsigned long)i);
        snx_a64_static_step(f, words[i].word, PC_BASE, name);
    }
    fputs("static StepFnPtr TABLE[]={", f);
    for (i = 0; i < count; ++i) fprintf(f, "%sS_%lu", i ? "," : "", (unsigned long)i);
    fputs("};\nSNX_EXPORT StepFnPtr* snx_test_steps(void){ return TABLE; }\n", f);
    fclose(f);
    abi = snx_path_join(repo, "src/common/cpu/aarch64");
    root = snx_path_join(repo, "src"); /* what cpu.h includes */
    c_native = snx_path_join(dir, "native.so");
    k = 0;
    args[k++] = clang;
    args[k++] = "-O2";
    args[k++] = "-shared";
    args[k++] = "-w";
    march = k;
#if defined(__aarch64__)
    args[k++] = "-march=armv8-a"; /* the reference: the helpers' plain C (no feature macros) */
#else
    args[k++] = "-march=x86-64";
    args[k++] = "-mcx16";
#endif
    args[k++] = "-fno-strict-aliasing";
    args[k++] = "-fwrapv";
#ifndef _WIN32
    args[k++] = "-fPIC";
#endif
    args[k++] = "-I";
    args[k++] = abi;
    args[k++] = "-I";
    args[k++] = root;
    args[k++] = c_path;
    args[k++] = "-o";
    args[k++] = c_lib;
    args[k] = NULL;
    if (!quiet) printf("%lu words; compiling the reference steps\n", (unsigned long)count);
    if (snx_run(args, NULL, NULL, &text)) return snx_error("portable compile failed:\n%s", text ? text : "");
    free(text);
    /* again with -march=native: the helpers' paths for the host's extensions (AES-NI, PCLMULQDQ, CRC32, GFNI) */
    args[march] = "-march=native";
    args[k - 1] = c_native;
    text = NULL;
    if (snx_run(args, NULL, NULL, &text)) return snx_error("native portable compile failed:\n%s", text ? text : "");
    free(text);
    /* the LLVM backend: snx_ir_static_step for each word, linked with the runtime library */
    if (!quiet) printf("compiling the LLVM steps\n");
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    ir = snx_ir_new("steps", '2', error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    if (portable) {
        static SnxIrFeatures plain_x86; /* x86-64, none of its extensions */
        plain_x86.x86 = ir->features->x86;
        ir->features = &plain_x86;
    }
    {
        LLVMValueRef *fns = (LLVMValueRef*)snx_alloc(count * sizeof(*fns)), table, getter;
        LLVMTypeRef type = LLVMArrayType(ir->t_ptr, (unsigned)count);
        for (i = 0; i < count; ++i) {
            char name[32];
            sprintf(name, "S_%lu", (unsigned long)i);
            fns[i] = snx_ir_static_step(ir, words[i].word, PC_BASE, name);
            LLVMSetVisibility(fns[i], LLVMHiddenVisibility);
        }
        table = LLVMAddGlobal(ir->mod, type, "TABLE");
        LLVMSetLinkage(table, LLVMInternalLinkage);
        LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, fns, (unsigned)count));
        getter = LLVMAddFunction(ir->mod, "snx_test_steps", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, getter, "entry"));
        LLVMBuildRet(ir->b, table);
        free(fns);
    }
    fallbacks = ir->fallbacks;
    if (snx_ir_object(ir, '2', &object, &object_size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    inputs[0].data = snx_rtlib_object;
    inputs[0].size = snx_rtlib_object_size;
    inputs[0].name = "rtlib";
    inputs[1].data = object;
    inputs[1].size = object_size;
    inputs[1].name = "steps";
    if (jit) {
        /* the runtime library first, as the JIT service loads it, then the steps calling into it */
        SnxU64 address;
        loader = snx_code_loader_new(0, error, sizeof(error));
        if (!loader) return snx_error("jit: %s", error);
        if (snx_code_load(loader, &inputs[0], 1, NULL, 0, NULL, error, sizeof(error)) ||
            snx_code_load(loader, &inputs[1], 1, exports, 1, &address, error, sizeof(error)))
            return snx_error("jit: %s", error);
        if (load_probe(loader)) return 1;
        table_l = (TableFn)(size_t)address;
        if (!quiet) printf("loaded %lu KiB of code\n", (unsigned long)(snx_code_used(loader) >> 10));
    } else {
        if (snx_link(inputs, 2, exports, 1, "llvm.so", &linked, error, sizeof(error)))
            return snx_error("link: %s", error);
        if (snx_write_file(l_lib, linked.data, linked.size)) return snx_error("cannot write %s", l_lib);
        free(linked.data);
    }
    free(object);
    if (!load(c_lib, &table_p) || !table_p) return snx_error("cannot load the reference steps");
    if (!load(c_native, &table_h) || !table_h) return snx_error("cannot load the native reference steps");
    steps_h = table_h();
    if (!jit && (!load(l_lib, &table_l) || !table_l)) return snx_error("cannot load the LLVM steps");
    steps_p = table_p();
    steps_l = table_l();
    if (gccjit) {
#ifdef SNX_TEST_GCCJIT
        char* g_lib = snx_path_join(dir, "gccjit.so");
        if (!quiet) printf("compiling the libgccjit steps\n");
        if (build_gccjit(words, count, dir, g_lib, &gccjit_fallbacks)) return 1;
        if (!load(g_lib, &table_g) || !table_g) return snx_error("cannot load the libgccjit steps");
        steps_g = table_g();
        free(g_lib);
#else
        return snx_error("built without libgccjit");
#endif
    }
    /* run */
    memset(&rt, 0, sizeof(rt));
    rt.svc = cb_svc;
    rt.undef = cb_undef;
    rt.note_hint = cb_note;
    rt.ticks = cb_ticks;
    rt.brk = cb_brk;
    rt.fpcr_changed = cb_fpcr;
    rt.sev = cb_sev;
    rt.wfe = cb_wfe;
    memory_start = (unsigned char*)malloc(MEMORY_SIZE);
    memory_p = (unsigned char*)malloc(MEMORY_SIZE);
    memory_l = (unsigned char*)malloc(MEMORY_SIZE);
    for (i = 0; i < MEMORY_SIZE; i += 8) {
        u64 r = special64();
        memcpy(memory_start + i, &r, 8);
    }
    for (i = 0; i < count; ++i) {
        size_t s;
        for (s = 0; s < states; ++s) {
            CPU start, p, l, h;
            Log log_p, log_l, log_h;
            u64 rp, rl, rh;
            int memory_differs, helper_memory;
            memset(&start, 0, sizeof(start));
            random_state(&start, memory_start, &words[i]);
            start.rt = &rt;
            p = l = start;
            memcpy(memory_p, memory_start, MEMORY_SIZE);
            memcpy(memory_l, memory_start, MEMORY_SIZE);
            p.mem = memory_p;
            l.mem = memory_l;
            memset(&log_p, 0, sizeof(log_p));
            memset(&log_l, 0, sizeof(log_l));
            current = &log_p;
            rp = steps_p[i](&p);
            current = &log_l;
            rl = steps_l[i](&l);
            memory_differs = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0;
            {
                h = start;
                memcpy(memory_l, memory_start, MEMORY_SIZE);
                h.mem = memory_l;
                memset(&log_h, 0, sizeof(log_h));
                current = &log_h;
                rh = steps_h[i](&h);
                helper_memory = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0; /* memory_l is reused: compared above */
                if (rp != rh || !same_cpu(&p, &h) || helper_memory || !same_events(&log_p, &log_h)) {
                    size_t index = (size_t)(words[i].pattern - patterns);
                    fprintf(stderr, "the helpers built with -march=native differ from the plain C ones:\n");
                    if (pattern_failures[index]++ < 3)
                        report(&words[i], &start, &p, &h, rp, rh, &log_p, &log_h, helper_memory);
                    ++failures;
                    break;
                }
            }
            ++compared;
            if (steps_g) { /* the libgccjit steps against the reference too */
                CPU g = start;
                Log log_g;
                u64 rg;
                memcpy(memory_l, memory_start, MEMORY_SIZE);
                g.mem = memory_l;
                memset(&log_g, 0, sizeof(log_g));
                current = &log_g;
                rg = steps_g[i](&g);
                helper_memory = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0;
                if (rp != rg || !same_cpu(&p, &g) || helper_memory || !same_events(&log_p, &log_g)) {
                    size_t index = (size_t)(words[i].pattern - patterns);
                    fprintf(stderr, "the libgccjit steps differ from the reference ones:\n");
                    if (pattern_failures[index]++ < 3)
                        report(&words[i], &start, &p, &g, rp, rg, &log_p, &log_g, helper_memory);
                    ++failures;
                    break;
                }
            }
            if (rp != rl || !same_cpu(&p, &l) || memory_differs || !same_events(&log_p, &log_l)) {
                size_t index = (size_t)(words[i].pattern - patterns);
                if (pattern_failures[index]++ < 3)
                    report(&words[i], &start, &p, &l, rp, rl, &log_p, &log_l, memory_differs);
                ++failures;
                break;
            }
        }
    }
    for (i = 0; i < npatterns; ++i)
        if (pattern_failures[i])
            printf("  %s: %lu words differ\n", patterns[i].name, (unsigned long)pattern_failures[i]);
    /* every pattern has an emitter, in both backends */
    fallbacks += gccjit_fallbacks;
    if (gccjit) printf("libgccjit: %lu without an emitter\n", gccjit_fallbacks);
    printf("LLVM differential%s: %lu words, %lu runs, %lu differing words, %lu without an emitter: %s\n",
           jit ? " (JIT loader)" : "", (unsigned long)count, (unsigned long)compared, (unsigned long)failures,
           fallbacks, failures || fallbacks ? "FAILED" : "passed");
    snx_remove_tree(dir);
    snx_code_loader_free(loader);
    return failures || fallbacks;
}
