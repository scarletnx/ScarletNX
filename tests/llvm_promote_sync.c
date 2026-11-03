/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ir_promote_registers' copies between the guest registers' locals and the CPU structure, at every kind of boundary:
   what the CPU structure holds wherever the runtime, a helper or another function can look at it, and what the code
   uses after one of them changed it. Each case is a guest program this file encodes word by word while it runs the
   same words on a C model of the registers, flags and guest memory. Every runtime callback (SVC, poll, miss, the FPCR
   hook) records the CPU structure and then changes the registers, as a runtime may: the records and the end state
   must match the model's. The program also runs compiled at O0, where nothing is promoted and every instruction
   reads and writes the CPU structure: the O2 run's records must match those field for field, the fields the model
   leaves out (the exclusive monitor's, an FP estimate's lanes) included.
   The registers: X0-X30, SP, NZCV, V0-V31 whole and by lane, FPCR, FPSR, W writes, the exclusive monitor. The
   boundaries: a return, an SVC, a direct BL, a BLR through dispatch, a backward branch's poll, an entry at another
   leader (the dispatch block), a join of two ways, a callee made stale (the AOT build's stale byte, a miss) and a
   caller made stale during a call, the helpers of each site class (ALL: MSR FPCR's hook; ENV: the exclusive
   monitor's; NONE: DC ZVA's and the FP helpers), and a register whose PHIs outnumber its accesses by more than 8, or
   whose copies as a local outweigh them, which stays in the CPU structure.
     snx-test-llvm-promote-sync [--portable]   (--portable: without the host's extensions, so the FP helpers run) */
#include "llvm_steps.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS   512
#define FUNCS   4
#define EVENTS  32
#define MEMORY  0x10000
#define DATA    0x8000 /* guest address of the programs' data: DC ZVA's blocks, the exclusive pair's word */
#define LR      30
#define KEEP_LR 0x80u  /* an SVC whose handler leaves X30 alone: one in a callee, which returns through it */
#define STALE   0x40u  /* an SVC whose handler makes the caller's code stale (RtAPI.code_changed's effect) */
#define KEEP_X9 0x100u /* an SVC whose handler leaves X9 alone too: one in a loop's callee, X9 the loop's counter */
#define POLLS   64     /* the polls a run may make: CPU.poll stays set, so a loop that never ends polls past them */

enum { EV_SVC, EV_POLL, EV_MISS, EV_FPCR, EV_END };
static const char* const kinds[] = {"svc", "poll", "miss", "fpcr hook", "end"};

/* the fields a comparison looks at */
enum {
    F_X = 1,
    F_SP = 2,
    F_PC = 4,
    F_NZCV = 8,
    F_V = 16,
    F_FPCR = 32,
    F_FPSR = 64,
    F_EXCL = 128,
    F_ALL = 255,
    F_MODEL = F_ALL & ~F_EXCL /* the monitor's encoding is the helpers' own: the O0 run checks it */
};

typedef struct {
    int kind;
    u64 arg;
    CPU state;
} Event;

typedef struct {
    Event events[EVENTS];
    size_t count;
    unsigned char* stale;     /* this module's stale bytes */
    unsigned char* ref_stale; /* the reference module's */
    GuestFn reference[FUNCS]; /* what a miss runs: the code at the pc compiled again (the O0 module's) */
    u64 starts[FUNCS], ends[FUNCS];
    GuestFn* disp;                  /* CPU.disp: a function made stale leaves it, as the runtime's fn_stale has it */
    size_t functions, stale_caller; /* an SVC with STALE makes this function stale */
    u64 poll_keep;                  /* the X registers the poll handler leaves alone */
    unsigned polls;                 /* so far: a loop that goes on past POLLS ends the test */
} Run;

/* a program and its model */
typedef struct {
    const char* name;
    u32 w[WORDS];
    size_t n, starts[FUNCS], nstarts;
    CPU m;                     /* the model's registers */
    unsigned char mem[MEMORY]; /* the model's guest memory */
    Event events[EVENTS];      /* what the model expects each callback to see */
    size_t count;
    unsigned fields;       /* what the model follows of the records and the end */
    u64 entry;             /* where the run enters: a function's start, or another leader of it */
    size_t entry_function; /* the function entry is in */
    int poll;              /* CPU.poll set at the start */
    size_t stale_callee;   /* the function made stale before the run, plus 1 (0: none) */
    size_t stale_caller;   /* the function an SVC with STALE makes stale */
    u64 poll_keep, skip_v; /* skip_v: the V registers the model leaves out (an FP estimate's result) */
    u64 memory_x, local_x; /* the X registers the case expects left in the CPU structure, and made locals */
    int call_stores;       /* at most these register stores before each call of call_target (-1: not checked) */
    size_t call_target;    /* the word a checked call goes to */
    int loop_x;            /* an X register no call of call_target's block may store (-1: none) */
} Program;

static struct {
    Program p;
    unsigned char mem2[MEMORY], mem0[MEMORY]; /* the O2 and O0 runs' guest memory */
    unsigned char image[WORDS * 4];           /* the program's words, as the module image holds them */
    int failures, cases;
} g_promote_sync;

static void fail(const char* name, const char* what) {
    fprintf(stderr, "FAIL %s: %s\n", name, what);
    ++g_promote_sync.failures;
}

/* ---- the state ---- */

static u64 mix(u64 v) {
    v ^= v >> 31;
    v *= 0x9E3779B97F4A7C15ull;
    v ^= v >> 29;
    v *= 0xBF58476D1CE4E5B9ull;
    return v ^ v >> 32;
}

/* the CPU structure and the memory every case starts from: a different value in every register, lane and byte */
static void initial(CPU* c) {
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < 32; ++i) {
        c->x[i] = mix(0x1000 + i);
        c->v[i].d[0] = mix(0x2000 + i);
        c->v[i].d[1] = mix(0x3000 + i);
    }
    c->n = 1;
    c->cf = 1;
    c->fpsr = 0x10; /* IXC */
}
static void initial_memory(unsigned char* m) {
    for (size_t i = 0; i < MEMORY; ++i) m[i] = (unsigned char)mix(i);
}

/* what a runtime callback does to the registers: every one changes, but the X registers in keep (bit r for Xr) */
static void mutate(CPU* c, unsigned salt, u64 keep) {
    for (unsigned i = 0; i < 32; ++i) {
        if (!(keep >> i & 1)) c->x[i] ^= mix(salt * 128 + i) | 1;
        c->v[i].d[0] ^= mix(salt * 128 + 32 + i) | 1;
        c->v[i].d[1] ^= mix(salt * 128 + 64 + i) | 1;
    }
    c->n ^= salt & 1;
    c->z ^= (salt >> 1 & 1) ^ 1;
    c->cf ^= salt >> 2 & 1;
    c->vf ^= (salt >> 3 & 1) ^ 1;
    c->fpsr ^= (u32)(mix(salt) & 0x0800009Fu) | 2;
}

/* the X registers an SVC's handler leaves alone, by its immediate */
static u64 keep_of(unsigned imm) { return (imm & KEEP_LR ? (u64)1 << LR : 0) | (imm & KEEP_X9 ? (u64)1 << 9 : 0); }

/* the architectural part of the CPU structure, the rest cleared (the runtime's pointers, the poll flag) */
static CPU state_of(const CPU* c) {
    CPU s = *c;
    s.mem = NULL;
    s.disp = NULL;
    s.code_lo = s.code_hi = 0;
    s.rt = NULL;
    s.thread = NULL;
    s.poll = 0;
    return s;
}

/* the first field of fields where a and b differ, as text in out; 0 when none does */
static int differ(const CPU* a, const CPU* b, unsigned fields, char* out, size_t size) {
    unsigned i;
    for (i = 0; i < 31; ++i)
        if (fields & F_X && a->x[i] != b->x[i]) {
            snprintf(out, size, "X%u %016llx, expected %016llx", i, (unsigned long long)a->x[i],
                     (unsigned long long)b->x[i]);
            return 1;
        }
    if (fields & F_SP && a->x[31] != b->x[31]) {
        snprintf(out, size, "SP %016llx, expected %016llx", (unsigned long long)a->x[31], (unsigned long long)b->x[31]);
        return 1;
    }
    if (fields & F_PC && a->pc != b->pc) {
        snprintf(out, size, "pc %llx, expected %llx", (unsigned long long)a->pc, (unsigned long long)b->pc);
        return 1;
    }
    if (fields & F_NZCV && (a->n != b->n || a->z != b->z || a->cf != b->cf || a->vf != b->vf)) {
        snprintf(out, size, "NZCV %u%u%u%u, expected %u%u%u%u", a->n, a->z, a->cf, a->vf, b->n, b->z, b->cf, b->vf);
        return 1;
    }
    for (i = 0; i < 32; ++i)
        if (fields & F_V && (a->v[i].d[0] != b->v[i].d[0] || a->v[i].d[1] != b->v[i].d[1])) {
            snprintf(out, size, "V%u %016llx:%016llx, expected %016llx:%016llx", i, (unsigned long long)a->v[i].d[1],
                     (unsigned long long)a->v[i].d[0], (unsigned long long)b->v[i].d[1],
                     (unsigned long long)b->v[i].d[0]);
            return 1;
        }
    if (fields & F_FPCR && a->fpcr != b->fpcr) {
        snprintf(out, size, "FPCR %08x, expected %08x", a->fpcr, b->fpcr);
        return 1;
    }
    if (fields & F_FPSR && a->fpsr != b->fpsr) {
        snprintf(out, size, "FPSR %08x, expected %08x", a->fpsr, b->fpsr);
        return 1;
    }
    if (fields & F_EXCL && (a->excl_addr != b->excl_addr || a->excl_val != b->excl_val ||
                            a->excl_val2 != b->excl_val2 || a->excl_size != b->excl_size || a->excl_on != b->excl_on)) {
        snprintf(out, size, "monitor %llx/%llx/%u/%u, expected %llx/%llx/%u/%u", (unsigned long long)a->excl_addr,
                 (unsigned long long)a->excl_val, a->excl_size, a->excl_on, (unsigned long long)b->excl_addr,
                 (unsigned long long)b->excl_val, b->excl_size, b->excl_on);
        return 1;
    }
    return 0;
}

/* ---- the encoder, which runs each word on the model as it writes it ---- */

static u64 at(size_t word) { return SNX_CODE_BASE + 4 * (u64)word; }
static void put(Program* p, u32 word) {
    if (p->n < WORDS) p->w[p->n] = word;
    ++p->n;
}
static void function(Program* p) {
    if (p->nstarts < FUNCS) p->starts[p->nstarts] = p->n;
    ++p->nstarts;
}
/* the model's value of a register operand where 31 is XZR (the shifted-register forms) */
static u64 xzr(const Program* p, unsigned r) { return r == 31 ? 0 : p->m.x[r]; }
static void expect(Program* p, int kind, u64 arg) {
    if (p->count < EVENTS) {
        p->events[p->count].kind = kind;
        p->events[p->count].arg = arg;
        p->events[p->count].state = p->m;
    }
    ++p->count;
}
static void mov64(Program* p, unsigned rd, u64 v) { /* MOVZ, then MOVK of each other half-word */
    for (unsigned hw = 0; hw < 4; ++hw)
        put(p, (hw ? 0xF2800000u : 0xD2800000u) | hw << 21 | (u32)(v >> 16 * hw & 0xFFFF) << 5 | rd);
    p->m.x[rd] = v;
}
static void mov(Program* p, unsigned rd, unsigned rm) { /* ORR Xd, XZR, Xm */
    put(p, 0xAA0003E0u | rm << 16 | rd);
    p->m.x[rd] = xzr(p, rm);
}
static void add_imm(Program* p, unsigned rd, unsigned rn, unsigned imm) { /* ADD Xd|SP, Xn|SP, #imm */
    put(p, 0x91000000u | imm << 10 | rn << 5 | rd);
    p->m.x[rd] = p->m.x[rn] + imm;
}
static void add_w(Program* p, unsigned rd, unsigned rn, unsigned imm) { /* ADD Wd, Wn, #imm: zero-extends */
    put(p, 0x11000000u | imm << 10 | rn << 5 | rd);
    p->m.x[rd] = (u32)(p->m.x[rn] + imm);
}
static void subs1_model(Program* p, unsigned rd) {
    u64 a = p->m.x[rd], r = a - 1;
    p->m.x[rd] = r;
    p->m.n = (u32)(r >> 63);
    p->m.z = r == 0;
    p->m.cf = a >= 1;
    p->m.vf = (u32)((a & ~r) >> 63); /* a - 1 overflows only from the most negative value */
}
static void subs1(Program* p, unsigned rd) { /* SUBS Xd, Xd, #1 */
    put(p, 0xF1000400u | rd << 5 | rd);
    subs1_model(p, rd);
}
static void msr_nzcv(Program* p, unsigned rt) {
    u64 v = p->m.x[rt];
    put(p, 0xD51B4200u | rt);
    p->m.n = (u32)(v >> 31 & 1);
    p->m.z = (u32)(v >> 30 & 1);
    p->m.cf = (u32)(v >> 29 & 1);
    p->m.vf = (u32)(v >> 28 & 1);
}
static void mrs_nzcv(Program* p, unsigned rt) {
    put(p, 0xD53B4200u | rt);
    p->m.x[rt] = (u64)p->m.n << 31 | (u64)p->m.z << 30 | (u64)p->m.cf << 29 | (u64)p->m.vf << 28;
}
static void msr_fpcr(Program* p, unsigned rt) { /* snx_msr: the implemented bits (no FP16), then the runtime's hook */
    put(p, 0xD51B4400u | rt);
    p->m.fpcr = (u32)p->m.x[rt] & 0x07C80000u & ~(1u << 19);
    expect(p, EV_FPCR, 0);
    mutate(&p->m, 0x21, 0);
}
static void mrs_fpcr(Program* p, unsigned rt) {
    put(p, 0xD53B4400u | rt);
    p->m.x[rt] = p->m.fpcr;
}
static void msr_fpsr(Program* p, unsigned rt) { /* the bits the A57 keeps */
    put(p, 0xD51B4420u | rt);
    p->m.fpsr = (u32)p->m.x[rt] & 0xF800009Fu;
}
static void mrs_fpsr(Program* p, unsigned rt) {
    put(p, 0xD53B4420u | rt);
    p->m.x[rt] = p->m.fpsr;
}
static void ins_d(Program* p, unsigned vd, unsigned lane, unsigned rn) { /* MOV Vd.D[lane], Xn */
    put(p, 0x4E081C00u | lane << 20 | rn << 5 | vd);
    p->m.v[vd].d[lane] = p->m.x[rn];
}
static void ins_s(Program* p, unsigned vd, unsigned lane, unsigned rn) { /* MOV Vd.S[lane], Wn: a part of Vd */
    put(p, 0x4E001C00u | (lane << 3 | 4) << 16 | rn << 5 | vd);
    p->m.v[vd].s[lane] = (u32)p->m.x[rn];
}
static void umov_s(Program* p, unsigned rd, unsigned vn, unsigned lane) { /* MOV Wd, Vn.S[lane] */
    put(p, 0x0E003C00u | (lane << 3 | 4) << 16 | vn << 5 | rd);
    p->m.x[rd] = p->m.v[vn].s[lane];
}
static void not16(Program* p, unsigned vd, unsigned vn) { /* MVN Vd.16B, Vn.16B */
    put(p, 0x6E205800u | vn << 5 | vd);
    p->m.v[vd].d[0] = ~p->m.v[vn].d[0];
    p->m.v[vd].d[1] = ~p->m.v[vn].d[1];
}
static void fmov_dx(Program* p, unsigned vd, unsigned rn) { /* FMOV Dd, Xn: the rest of Vd cleared */
    put(p, 0x9E670000u | rn << 5 | vd);
    p->m.v[vd].d[0] = p->m.x[rn];
    p->m.v[vd].d[1] = 0;
}
/* the model's side of an SVC at word: pc+4, the record, the handler's changes */
static void svc_model(Program* p, size_t word, unsigned imm) {
    p->m.pc = at(word) + 4;
    expect(p, EV_SVC, imm);
    mutate(&p->m, imm, keep_of(imm));
}
static void svc(Program* p, unsigned imm) {
    size_t word = p->n;
    put(p, 0xD4000001u | imm << 5);
    svc_model(p, word, imm);
}
static void ret(Program* p, unsigned rn) {
    put(p, 0xD65F0000u | rn << 5);
    p->m.pc = p->m.x[rn];
}
static void dczva(Program* p, unsigned rt) {
    put(p, 0xD50B7420u | rt);
    memset(p->mem + (p->m.x[rt] & ~(u64)63), 0, 64);
}
static u64 rd64(const unsigned char* m, u64 a) {
    u64 v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (u64)m[a + i] << 8 * i;
    return v;
}
static void wr64(unsigned char* m, u64 a, u64 v) {
    for (unsigned i = 0; i < 8; ++i) m[a + i] = (unsigned char)(v >> 8 * i);
}
static void ldxr(Program* p, unsigned rt, unsigned rn) {
    put(p, 0xC85F7C00u | rn << 5 | rt);
    p->m.x[rt] = rd64(p->mem, p->m.x[rn]);
}
static void stxr(Program* p, unsigned rs, unsigned rt, unsigned rn) { /* succeeds: nothing touched the word since */
    put(p, 0xC8007C00u | rs << 16 | rn << 5 | rt);
    wr64(p->mem, p->m.x[rn], p->m.x[rt]);
    p->m.x[rs] = 0;
}
/* the branch at word to target, by its imm26 (B, BL) or imm19 (B.cond, CBZ) */
static void patch(Program* p, size_t word, size_t target) {
    u32 off = (u32)(target - word), w = p->w[word];
    if ((w & 0x7C000000u) == 0x14000000u)
        p->w[word] = (w & 0xFC000000u) | (off & 0x03FFFFFFu);
    else
        p->w[word] = (w & 0xFF00001Fu) | (off & 0x7FFFFu) << 5;
}

/* the X registers a case writes and reads around a boundary: all but the link register and those in keep */
static int plain(unsigned r, u64 keep) { return r != LR && !(keep >> r & 1); }
/* every register written: each plain X, SP, NZCV (from the first plain X), every V */
static void dirty_all(Program* p, u64 keep, unsigned salt) {
    unsigned r;
    for (r = 0; r < 31; ++r)
        if (plain(r, keep)) add_imm(p, r, r, 1 + (salt * 7 + r) % 4000);
    add_imm(p, 31, 31, 16);
    for (r = 0; r < 31 && !plain(r, keep); ++r) {}
    mov64(p, r, p->m.x[r] ^ 0xA0000000u); /* N and C, as the bits 31:28 have them */
    msr_nzcv(p, r);
    for (unsigned v = 0; v < 32; ++v) not16(p, v, v);
}
/* every register read after a boundary: each plain X and SP added to, every V inverted, NZCV and FPSR copied into
   the first two plain X */
static void use_all(Program* p, u64 keep) {
    unsigned r, first = 32, second = 32;
    for (r = 0; r < 31; ++r)
        if (plain(r, keep)) {
            if (first == 32)
                first = r;
            else if (second == 32)
                second = r;
            add_imm(p, r, r, 3);
        }
    add_imm(p, 31, 31, 32);
    for (unsigned v = 0; v < 32; ++v) not16(p, v, v);
    mrs_nzcv(p, first);
    mrs_fpsr(p, second);
}

/* ---- running a program ---- */

static Run* run_of(CPU* c) { return (Run*)c->thread; }
static void record(CPU* c, int kind, u64 arg) {
    Run* r = run_of(c);
    if (r->count < EVENTS) {
        r->events[r->count].kind = kind;
        r->events[r->count].arg = arg;
        r->events[r->count].state = state_of(c);
    }
    ++r->count;
}
static void on_svc(CPU* c, u32 imm) {
    Run* r = run_of(c);
    record(c, EV_SVC, imm);
    mutate(c, imm, keep_of(imm));
    if (imm & STALE) { /* the runtime's order: its dispatch slots first, then its byte */
        for (u64 pc = r->starts[r->stale_caller]; pc < r->ends[r->stale_caller]; pc += 4)
            r->disp[(pc - SNX_CODE_BASE) / 4] = NULL;
        r->stale[r->stale_caller] = 1;
    }
}
/* CPU.poll stays set: every backward branch polls, and so does the dispatch block the poll's call of the function
   enters through. Each poll changes the registers by its own salt */
static void on_poll(CPU* c) {
    Run* r = run_of(c);
    record(c, EV_POLL, c->pc);
    if (++r->polls > POLLS) {
        fprintf(stderr, "FAIL: %u polls, and the loop goes on: its counter is not the model's\n", r->polls);
        exit(1);
    }
    mutate(c, 0x30 + r->polls, r->poll_keep);
}
static void on_fpcr(CPU* c) {
    record(c, EV_FPCR, 0);
    mutate(c, 0x21, 0);
}
/* the runtime's answer for a pc whose code is out of dispatch: the code there compiled again, the reference module's
   function holding pc entered at pc (its dispatch block takes it to the leader), with its own stale byte clear while
   it runs */
static void on_miss(CPU* c, u64 pc) {
    Run* r = run_of(c);
    record(c, EV_MISS, pc);
    for (size_t i = 0; i < r->functions; ++i)
        if (r->starts[i] <= pc && pc < r->ends[i]) {
            unsigned char was = r->ref_stale[i];
            r->ref_stale[i] = 0;
            r->reference[i](c, pc);
            r->ref_stale[i] = was;
            return;
        }
    fprintf(stderr, "a miss at %llx, where no function is\n", (unsigned long long)pc);
    exit(3);
}
static void on_undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    fprintf(stderr, "undefined %08x at %llx\n", w, (unsigned long long)pc);
    exit(3);
}
static void on_brk(CPU* c, u64 pc, u32 imm) { on_undef(c, pc, imm); }

typedef struct {
    LlvmSteps steps;
    GuestFn fns[FUNCS];
    unsigned char* stale;
    int begun, ok;
} Module;

/* a load of the CPU structure at offset in a block with no call: the register is read where it is used, not copied
   into a local on entry or after a call (ir_promote.c leaves a register in the CPU structure when its PHIs outnumber
   its accesses by more than 8) */
static int loaded_apart_from_calls(LLVMValueRef fn, size_t offset) {
    LLVMValueRef c = LLVMGetParam(fn, 0);
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        int calls = 0, loads = 0;
        for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst)) {
            LLVMValueRef ptr;
            calls += LLVMIsACallInst(inst) != NULL;
            if (!LLVMIsALoadInst(inst)) continue;
            ptr = LLVMGetOperand(inst, 0);
            loads += LLVMIsAGetElementPtrInst(ptr) && LLVMGetOperand(ptr, 0) == c && LLVMGetNumOperands(ptr) == 2 &&
                     LLVMIsAConstantInt(LLVMGetOperand(ptr, 1)) &&
                     LLVMConstIntGetZExtValue(LLVMGetOperand(ptr, 1)) == offset;
        }
        if (loads && !calls) return 1;
    }
    return 0;
}

/* the register stores before a call of target in its block, the most of any such call; with offset < 0x388 only
   those of that field */
static int stores_before(LLVMValueRef fn, LLVMValueRef target, size_t offset) {
    LLVMValueRef c = LLVMGetParam(fn, 0);
    int most = -1;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        int n = 0;
        for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst)) {
            if (LLVMIsACallInst(inst)) {
                if (LLVMGetCalledValue(inst) == target && n > most) most = n;
                n = 0;
            } else if (LLVMIsAStoreInst(inst)) {
                LLVMValueRef ptr = LLVMGetOperand(inst, 1);
                size_t at = SIZE_MAX;
                if (ptr == c)
                    at = 0;
                else if (LLVMIsAGetElementPtrInst(ptr) && LLVMGetOperand(ptr, 0) == c && LLVMGetNumOperands(ptr) == 2 &&
                         LLVMIsAConstantInt(LLVMGetOperand(ptr, 1)))
                    at = (size_t)LLVMConstIntGetZExtValue(LLVMGetOperand(ptr, 1));
                if (at < offsetof(CPU, tpidr) && (at < offsetof(CPU, pc) || at >= offsetof(CPU, n)) &&
                    (offset >= sizeof(CPU) || at == offset))
                    ++n;
            }
        }
    }
    return most;
}

/* the promoted function's shape the case expects: registers left in the CPU structure or made locals, the stores at
   a call of call_target */
static void structure(const Program* p, SnxIr* ir, LLVMValueRef fn) {
    char what[160];
    for (unsigned r = 0; r < 31; ++r) {
        int memory = loaded_apart_from_calls(fn, offsetof(CPU, x) + 8 * (size_t)r);
        if (p->memory_x >> r & 1 && !memory) {
            snprintf(what, sizeof(what), "X%u is made a local, though the case leaves it in the CPU structure", r);
            fail(p->name, what);
        }
        if (p->local_x >> r & 1 && memory) {
            snprintf(what, sizeof(what), "X%u is left in the CPU structure, though the case makes it a local", r);
            fail(p->name, what);
        }
    }
    if (p->call_stores >= 0 || p->loop_x >= 0) {
        LLVMValueRef target = snx_ir_guest_function(ir, at(p->call_target));
        int n = stores_before(fn, target, p->loop_x >= 0 ? offsetof(CPU, x) + 8 * (size_t)p->loop_x : SIZE_MAX);
        if (p->call_stores >= 0 && n > p->call_stores) {
            snprintf(what, sizeof(what), "%d register stores before a call of F_%llX, at most %d expected", n,
                     (unsigned long long)at(p->call_target), p->call_stores);
            fail(p->name, what);
        }
        if (p->loop_x >= 0 && n > 0) {
            snprintf(what, sizeof(what), "X%d is stored before the loop's call on every turn", p->loop_x);
            fail(p->name, what);
        }
    }
}

/* the program's functions compiled at opt with the AOT build's stale bytes, a module global the table's last entry
   points at */
static void compile(Module* mod, const Program* p, SnxCodegen* codegen, char opt, int portable) {
    char error[1024];
    LLVMValueRef stale;
    LLVMTypeRef bytes;
    size_t i, count = codegen->function_count;
    memset(mod, 0, sizeof(*mod));
    if (llvm_steps_begin_opt(&mod->steps, 1, portable, opt, error, sizeof(error))) {
        fprintf(stderr, "%s: %s\n", p->name, error);
        fail(p->name, "start the module");
        return;
    }
    mod->begun = 1;
    bytes = LLVMArrayType(mod->steps.ir->t_i8, FUNCS);
    stale = LLVMAddGlobal(mod->steps.ir->mod, bytes, SNX_IR_STALE);
    LLVMSetInitializer(stale, LLVMConstNull(bytes));
    LLVMSetVisibility(stale, LLVMHiddenVisibility);
    snx_ir_stale_checks(mod->steps.ir);
    for (i = 0; i < count; ++i) llvm_steps_add_function(&mod->steps, codegen, &codegen->functions[i]);
    if (opt != '0') structure(p, mod->steps.ir, mod->steps.fns[p->entry_function]);
    if (mod->steps.count == mod->steps.cap) {
        mod->steps.cap = mod->steps.cap ? mod->steps.cap * 2 : 8;
        mod->steps.fns = (LLVMValueRef*)realloc(mod->steps.fns, mod->steps.cap * sizeof(*mod->steps.fns));
    }
    mod->steps.fns[mod->steps.count++] = stale;
    if (llvm_steps_load(&mod->steps, error, sizeof(error))) {
        fprintf(stderr, "%s: %s\n", p->name, error);
        fail(p->name, "compile and load the module");
        return;
    }
    for (i = 0; i < count; ++i) mod->fns[i] = (GuestFn)(void (*)(void))mod->steps.steps[i];
    mod->stale = (unsigned char*)(void*)mod->steps.steps[count];
    mod->ok = 1;
}

/* one run of the program on mod, a miss running ref's code: the records in r, the end in end, the memory in memory */
static void run(const Program* p, Module* mod, Module* ref, SnxCodegen* codegen, Run* r, CPU* end,
                unsigned char* memory) {
    static const RtAPI rt = {
        .svc = on_svc, .undef = on_undef, .brk = on_brk, .fpcr_changed = on_fpcr, .poll = on_poll, .miss = on_miss};
    GuestFn disp[WORDS] = {0};
    CPU c;
    size_t i;
    memset(r, 0, sizeof(*r));
    r->functions = codegen->function_count;
    for (i = 0; i < r->functions; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        r->starts[i] = f->start;
        r->ends[i] = SNX_CODE_BASE +
                     4 * (u64)(i + 1 < r->functions ? (codegen->functions[i + 1].start - SNX_CODE_BASE) / 4 : p->n);
        r->reference[i] = ref->fns[i];
        for (size_t k = 0; k < f->count; ++k) /* every leader, as the module's entries are */
            if (f->leaders[k] && p->stale_callee != i + 1)
                disp[(snx_function_address(f, k) - SNX_CODE_BASE) / 4] = mod->fns[i];
    }
    r->disp = disp;
    memset(mod->stale, 0, FUNCS);
    memset(ref->stale, 0, FUNCS);
    if (p->stale_callee) mod->stale[p->stale_callee - 1] = ref->stale[p->stale_callee - 1] = 1;
    r->stale = mod->stale;
    r->ref_stale = ref->stale;
    r->stale_caller = p->stale_caller;
    r->poll_keep = p->poll_keep;
    initial_memory(memory);
    initial(&c);
    c.rt = &rt;
    c.mem = memory;
    c.disp = disp;
    c.code_lo = SNX_CODE_BASE;
    c.code_hi = SNX_CODE_BASE + 4 * (u64)p->n;
    c.thread = r;
    c.poll = (u32)p->poll;
    mod->fns[p->entry_function](&c, p->entry);
    /* the runtime's thread loop: what the code left in CPU.pc runs through dispatch, or a miss */
    for (i = 0; c.pc >= c.code_lo && c.pc < c.code_hi; ++i) {
        GuestFn f = disp[(c.pc - SNX_CODE_BASE) / 4];
        if (i == 64) {
            fail(p->name, "the dispatch loop goes on past 64 returns to it");
            break;
        }
        (f ? f : on_miss)(&c, c.pc);
    }
    *end = state_of(&c);
}

/* the records of a run against the model's (its fields) and against the reference run's (every field) */
static void compare(const Program* p, const Run* got, const Run* ref, const char* level) {
    char what[512], text[256];
    if (got->count != p->count) {
        snprintf(what, sizeof(what), "%zu records at %s, expected %zu", got->count, level, p->count);
        fail(p->name, what);
    }
    for (size_t i = 0; i < p->count && i < got->count && i < EVENTS; ++i) {
        const Event *want = &p->events[i], *e = &got->events[i];
        CPU masked = want->state;
        for (unsigned v = 0; v < 32; ++v)
            if (p->skip_v >> v & 1) masked.v[v] = e->state.v[v];
        if (e->kind != want->kind || e->arg != want->arg) {
            snprintf(what, sizeof(what), "record %zu at %s: %s %llx, expected %s %llx", i, level, kinds[e->kind],
                     (unsigned long long)e->arg, kinds[want->kind], (unsigned long long)want->arg);
            fail(p->name, what);
            return;
        }
        if (differ(&e->state, &masked, p->fields, text, sizeof(text))) {
            snprintf(what, sizeof(what), "record %zu (%s) at %s: %s, the model's", i, kinds[want->kind], level, text);
            fail(p->name, what);
        }
        if (ref && i < ref->count && differ(&e->state, &ref->events[i].state, F_ALL, text, sizeof(text))) {
            snprintf(what, sizeof(what), "record %zu (%s) at %s: %s, O0's", i, kinds[want->kind], level, text);
            fail(p->name, what);
        }
    }
}
static void compare_end(const Program* p, const CPU* end, const unsigned char* memory, const CPU* ref,
                        const char* level) {
    char what[512], text[256];
    CPU masked = p->m;
    for (unsigned v = 0; v < 32; ++v)
        if (p->skip_v >> v & 1) masked.v[v] = end->v[v];
    if (differ(end, &masked, p->fields, text, sizeof(text))) {
        snprintf(what, sizeof(what), "the end at %s: %s, the model's", level, text);
        fail(p->name, what);
    }
    if (ref && differ(end, ref, F_ALL, text, sizeof(text))) {
        snprintf(what, sizeof(what), "the end at %s: %s, O0's", level, text);
        fail(p->name, what);
    }
    for (size_t i = 0; i < MEMORY; ++i)
        if (memory[i] != p->mem[i]) {
            snprintf(what, sizeof(what), "guest memory at %s: byte %zx is %02x, expected %02x", level, i, memory[i],
                     p->mem[i]);
            fail(p->name, what);
            break;
        }
}

static void check(Program* p, int portable) {
    unsigned char* image = g_promote_sync.image;
    SnxProgram program = {0};
    SnxModuleImage module = {0};
    SnxCodegen codegen;
    SnxFde ranges[FUNCS];
    Module o2, o0;
    Run r2, r0;
    CPU e2, e0;
    size_t i;
    ++g_promote_sync.cases;
    if (p->n > WORDS || p->count > EVENTS || !p->nstarts || p->nstarts > FUNCS) {
        fail(p->name, "the program is larger than the test's arrays");
        return;
    }
    for (i = 0; i < p->n; ++i)
        for (unsigned b = 0; b < 4; ++b) image[4 * i + b] = (unsigned char)(p->w[i] >> 8 * b);
    module.name = (char*)"promote_sync"; /* the loader's field, never written through here */
    module.image = image;
    module.image_size = 4 * p->n;
    module.base = SNX_CODE_BASE;
    module.size = 0x1000;
    module.text_size = (SnxU32)(4 * p->n);
    for (i = 0; i < p->nstarts; ++i) {
        ranges[i].start = 4 * (SnxU64)p->starts[i];
        ranges[i].size = 4 * (SnxU64)((i + 1 < p->nstarts ? p->starts[i + 1] : p->n) - p->starts[i]);
    }
    module.fde = ranges;
    module.fde_count = p->nstarts;
    program.modules = &module;
    program.module_count = 1;
    program.npdm.title_id = 0x123456789ABCDEF0ull;
    program.npdm.stack = 0x100000;
    program.npdm.prio = 44;
    program.npdm.core = 2;
    if (snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1)) {
        fail(p->name, "analyze the program");
        return;
    }
    if (codegen.function_count != p->nstarts) {
        char what[128];
        snprintf(what, sizeof(what), "%zu functions found, the program has %zu", codegen.function_count, p->nstarts);
        fail(p->name, what);
        snx_codegen_free(&codegen);
        return;
    }
    compile(&o2, p, &codegen, '2', portable);
    compile(&o0, p, &codegen, '0', portable);
    if (o2.ok && o0.ok) {
        run(p, &o0, &o0, &codegen, &r0, &e0, g_promote_sync.mem0);
        run(p, &o2, &o0, &codegen, &r2, &e2, g_promote_sync.mem2);
        compare(p, &r0, NULL, "O0");
        compare(p, &r2, &r0, "O2");
        compare_end(p, &e0, g_promote_sync.mem0, NULL, "O0");
        compare_end(p, &e2, g_promote_sync.mem2, &e0, "O2");
    }
    if (o2.begun) llvm_steps_end(&o2.steps);
    if (o0.begun) llvm_steps_end(&o0.steps);
    snx_codegen_free(&codegen);
}

/* ---- the cases ---- */

static Program* begin(const char* name) {
    Program* p = &g_promote_sync.p;
    memset(p, 0, sizeof(*p));
    p->name = name;
    initial(&p->m);
    initial_memory(p->mem);
    p->fields = F_MODEL;
    p->entry = SNX_CODE_BASE;
    p->call_stores = -1;
    p->loop_x = -1;
    function(p);
    return p;
}
/* the callee the calling cases share, written first (function 0): SVC (its handler keeping X30, and with stale also
   making the caller stale), X3 += 5, V7.D[1] = X3, RET. Its word of SVC */
static size_t callee(Program* p, unsigned extra) {
    size_t word = p->n;
    put(p, 0xD4000001u | (5u | KEEP_LR | extra) << 5);
    put(p, 0x91001463u); /* ADD X3, X3, #5 */
    put(p, 0x4E181C67u); /* MOV V7.D[1], X3 */
    put(p, 0xD65F03C0u); /* RET */
    function(p);
    p->entry = at(p->n);
    p->entry_function = 1;
    return word;
}
/* what the callee does, on the model, called at word (a BL or BLR) */
static void callee_model(Program* p, size_t call, size_t svc_word, unsigned extra) {
    p->m.x[LR] = at(call) + 4;
    svc_model(p, svc_word, 5u | KEEP_LR | extra);
    p->m.x[3] += 5;
    p->m.v[7].d[1] = p->m.x[3];
    p->m.pc = p->m.x[LR];
}
static void bl(Program* p, size_t target) {
    size_t word = p->n;
    put(p, 0x94000000u);
    patch(p, word, target);
}
static size_t branch_if(Program* p, unsigned cond) { /* B.cond, its target patched later */
    size_t word = p->n;
    put(p, 0x54000000u | cond);
    return word;
}

static void cases(int portable) {
    Program* p;
    size_t s, w, k;
    /* RET: every register written, the return stores them */
    p = begin("return");
    dirty_all(p, 0, 1);
    add_imm(p, LR, LR, 8);
    ret(p, LR);
    check(p, portable);
    /* an SVC: stored before it, reloaded after it where read */
    p = begin("svc");
    dirty_all(p, 0, 2);
    svc(p, 1);
    use_all(p, 0);
    svc(p, 2);
    add_imm(p, LR, LR, 4);
    ret(p, LR);
    check(p, portable);
    /* a direct BL: the callee's SVC sees the caller's registers, the caller reads what the callee changed */
    p = begin("bl");
    s = callee(p, 0);
    dirty_all(p, 0, 3);
    w = p->n;
    bl(p, s);
    callee_model(p, w, s, 0);
    use_all(p, 0);
    svc(p, 3);
    ret(p, LR);
    check(p, portable);
    /* a BLR through dispatch (snx_lookup, CPU.disp) */
    p = begin("blr");
    s = callee(p, 0);
    dirty_all(p, 1u << 16, 4);
    mov64(p, 16, at(s));
    w = p->n;
    put(p, 0xD63F0200u); /* BLR X16 */
    callee_model(p, w, s, 0);
    use_all(p, 1u << 16);
    svc(p, 4);
    ret(p, LR);
    check(p, portable);
    /* DC ZVA's helper is no site (NONE): the registers stay locals across it, the next SVC sees them */
    p = begin("dc zva");
    dirty_all(p, 1u << 2, 5);
    mov64(p, 2, DATA + 0x140);
    dczva(p, 2);
    svc(p, 6);
    use_all(p, 1u << 2);
    mov64(p, 2, DATA + 0x140); /* the SVC changed it */
    dczva(p, 2);
    svc(p, 7);
    ret(p, LR);
    check(p, portable);
    /* the exclusive monitor's helpers (ENV: FPCR and FPSR alone), a DC ZVA of another block between the pair, and a
       load-exclusive whose monitor an SVC sees */
    p = begin("exclusive");
    dirty_all(p, 1u << 2 | 1u << 4, 6);
    mov64(p, 2, DATA);
    mov64(p, 4, DATA + 0x1000);
    ldxr(p, 3, 2);
    dczva(p, 4);
    stxr(p, 6, 5, 2);
    svc(p, 8);
    use_all(p, 1u << 2 | 1u << 4);
    mov64(p, 2, DATA); /* the SVC changed it */
    ldxr(p, 7, 2);
    svc(p, 9);
    ret(p, LR);
    check(p, portable);
    /* FPSR by MSR (an ALL site), a division by zero (DZC), and FRECPE (a lane helper given locals without the host's
       extensions: NONE), whose estimate the model leaves to the O0 run */
    p = begin("fp helpers");
    mov64(p, 9, 0x08000001u); /* QC and IOC */
    msr_fpsr(p, 9);
    mov64(p, 10, 0x3FF0000000000000ull);
    fmov_dx(p, 3, 10);
    mov64(p, 11, 0);
    fmov_dx(p, 4, 11);
    put(p, 0x1E641862u); /* FDIV D2, D3, D4: 1/+0 */
    p->m.v[2].d[0] = 0x7FF0000000000000ull;
    p->m.v[2].d[1] = 0;
    p->m.fpsr |= 2;
    mov64(p, 12, 0x4000000040000000ull); /* 2.0f in each lane: an estimate that raises no flag */
    ins_d(p, 1, 0, 12);
    ins_d(p, 1, 1, 12);
    put(p, 0x4EA1D820u); /* FRECPE V0.4S, V1.4S */
    p->skip_v = 1;
    svc(p, 10);
    use_all(p, 0);
    svc(p, 11);
    ret(p, LR);
    check(p, portable);
    /* MSR FPCR: snx_msr calls the runtime's FPCR hook, which sees every register (ALL) and changes them */
    p = begin("fpcr hook");
    dirty_all(p, 1u << 4, 7);
    mov64(p, 4, 0x02000000u); /* DN */
    msr_fpcr(p, 4);
    use_all(p, 0);
    mrs_fpcr(p, 5);
    svc(p, 12);
    ret(p, LR);
    check(p, portable);
    /* an entry at another leader: the dispatch block's switch on CPU.pc, from the registers in the CPU structure */
    p = begin("dispatch entry");
    put(p, 0x91000421u); /* ADD X1, X1, #1 */
    k = p->n;
    put(p, 0xB4000002u); /* CBZ X2, the leader */
    put(p, 0x91000821u); /* ADD X1, X1, #2 */
    patch(p, k, p->n);
    p->entry = at(p->n);
    p->m.pc = p->entry;
    use_all(p, 0);
    svc(p, 15);
    ret(p, LR);
    check(p, portable);
    /* a callee made stale (the AOT build's stale byte, its dispatch entry gone): its entry goes through snx_lookup to a
       miss, which sees every register and runs the code there compiled again */
    p = begin("stale callee");
    s = callee(p, 0);
    p->stale_callee = 1;
    dirty_all(p, 0, 9);
    w = p->n;
    bl(p, s);
    p->m.x[LR] = at(w) + 4;
    expect(p, EV_MISS, at(s));
    callee_model(p, w, s, 0);
    use_all(p, 0);
    svc(p, 16);
    ret(p, LR);
    check(p, portable);
    /* a caller made stale while its callee ran: its return check leaves at once, with the registers as the call left
       them in the CPU structure and CPU.pc after the BL, and the dispatch loop misses there, into the code compiled
       again */
    p = begin("stale caller");
    s = callee(p, STALE);
    p->stale_caller = 1;
    dirty_all(p, 0, 10);
    w = p->n;
    bl(p, s);
    callee_model(p, w, s, STALE);
    expect(p, EV_MISS, at(w) + 4);
    use_all(p, 0);
    svc(p, 17);
    ret(p, LR);
    check(p, portable);
    /* X20 live across twelve calls, each on one side of a branch, and read once at the end: more PHIs than accesses
       by over 8, so it stays in the CPU structure (ir_promote.c), where every call's change must reach its read */
    p = begin("phis");
    s = callee(p, 0);
    p->memory_x = (u64)1 << 20;
    mov(p, 19, LR);
    for (unsigned i = 0; i < 12; ++i) {
        size_t skip = p->n;
        put(p, 0xB4000016u); /* CBZ X22 past the call */
        if (!p->m.x[22]) fail(p->name, "X22 is 0: the model takes no call");
        w = p->n;
        bl(p, s);
        callee_model(p, w, s, 0);
        patch(p, skip, p->n);
    }
    add_imm(p, 0, 20, 1);
    ret(p, 19);
    check(p, portable);
    /* the writes in one block and the SVC in another, after a join of two ways (only one of them writes X1): the
       dirty registers carried across the blocks */
    p = begin("join");
    dirty_all(p, 0, 11);
    k = p->n;
    put(p, 0xB4000000u | 22); /* CBZ X22 to the join: X22 is not 0, so the ADD runs */
    if (!p->m.x[22]) fail(p->name, "X22 is 0: the model takes the other way");
    add_imm(p, 1, 1, 9);
    patch(p, k, p->n);
    svc(p, 22);
    use_all(p, 0);
    k = p->n;
    put(p, 0xB4000000u | 23); /* CBZ X23 past one more write */
    if (!p->m.x[23]) fail(p->name, "X23 is 0: the model takes the other way");
    add_imm(p, 2, 2, 5);
    patch(p, k, p->n);
    svc(p, 23);
    ret(p, LR);
    check(p, portable);
    /* lanes: a part of V5 written (the rest read), the SVC's change to another lane read after it */
    p = begin("lanes");
    ins_s(p, 5, 2, 7);
    ins_d(p, 6, 1, 8);
    svc(p, 18);
    umov_s(p, 9, 5, 1);
    ins_s(p, 5, 3, 10);
    svc(p, 19);
    ret(p, LR);
    check(p, portable);
    /* W writes (zero-extended) and SP across SVCs */
    p = begin("w and sp");
    add_w(p, 4, 5, 7);
    add_w(p, 6, 6, 1);
    add_imm(p, 31, 31, 64);
    add_imm(p, 8, 31, 16);
    svc(p, 20);
    add_w(p, 4, 4, 1);
    add_imm(p, 31, 31, 16);
    svc(p, 21);
    ret(p, LR);
    check(p, portable);
    /* loops 1, 2 and 3 deep after six joins around calls: X20, X23 and X24 have PHIs at every join and few
       accesses, but those in the loops weigh by their depth, so they are made locals; X21, read once outside them,
       stays in the CPU structure. X25, read once in the outer loop, outweighs its reloads after the calls before it
       at 16 times a level, not at 4 */
    p = begin("loop weight");
    s = callee(p, 0);
    p->memory_x = (u64)1 << 21;
    p->local_x = (u64)1 << 20 | (u64)1 << 23 | (u64)1 << 24 | (u64)1 << 25;
    mov(p, 19, LR);
    for (unsigned i = 0; i < 6; ++i) {
        size_t skip = p->n;
        put(p, 0xB4000016u); /* CBZ X22 past the call */
        if (!p->m.x[22]) fail(p->name, "X22 is 0: the model takes no call");
        w = p->n;
        bl(p, s);
        callee_model(p, w, s, 0);
        patch(p, skip, p->n);
    }
    {
        CPU before = p->m; /* the loops' words, then what their eight turns do */
        size_t l1, l2, l3;
        mov64(p, 9, 2);
        l1 = p->n;
        add_imm(p, 20, 20, 1);
        add_imm(p, 26, 25, 1);
        mov64(p, 10, 2);
        l2 = p->n;
        add_imm(p, 23, 23, 1);
        mov64(p, 11, 2);
        l3 = p->n;
        add_imm(p, 24, 24, 1);
        subs1(p, 11);
        patch(p, branch_if(p, 1), l3); /* B.NE */
        add_imm(p, 23, 23, 2);
        subs1(p, 10);
        patch(p, branch_if(p, 1), l2);
        add_imm(p, 20, 20, 2);
        subs1(p, 9);
        patch(p, branch_if(p, 1), l1);
        p->m = before;
        p->m.x[20] += 2 * 3;
        p->m.x[26] = p->m.x[25] + 1;
        p->m.x[23] += 2 * 2 * 3;
        p->m.x[24] += 2 * 2 * 2;
        p->m.x[9] = p->m.x[10] = p->m.x[11] = 0;
        p->m.n = 0, p->m.z = 1, p->m.cf = 1, p->m.vf = 0; /* SUBS 1-1 */
    }
    add_imm(p, 0, 21, 1);
    ret(p, 19);
    check(p, portable);
    /* X21 written before a loop whose call sees every register, and read after the loop: as a local it would be
       reloaded after the call on every turn, in memory it is loaded once, so it stays in the CPU structure. X22,
       written before the call and added to after it on every turn, is a local */
    p = begin("copies");
    s = callee(p, KEEP_X9);
    p->memory_x = (u64)1 << 21;
    p->local_x = (u64)1 << 22;
    mov(p, 20, LR);
    add_imm(p, 21, 21, 5);
    mov64(p, 9, 3);
    {
        size_t head = p->n;
        mov(p, 22, 1);
        w = p->n;
        bl(p, s);
        callee_model(p, w, s, KEEP_X9);
        add_imm(p, 22, 22, 1);
        subs1(p, 9);
        patch(p, branch_if(p, 1), head);
        for (unsigned turn = 1; turn < 3; ++turn) {
            p->m.x[22] = p->m.x[1];
            callee_model(p, w, s, KEEP_X9);
            p->m.x[22] += 1;
            subs1_model(p, 9);
        }
    }
    add_imm(p, 0, 21, 1);
    ret(p, 20);
    check(p, portable);
    /* last: a loop whose counter goes wrong polls past POLLS, which ends the test */
    /* a backward branch's poll with CPU.poll set: stored before rt->poll, which changes them, and the loop goes on
       through the function's entry and dispatch block (which polls again) from the CPU structure. Each of the two
       backward branches the loop takes polls twice */
    p = begin("poll");
    p->poll = 1;
    p->poll_keep = 1u << 9;
    dirty_all(p, 1u << 9, 8);
    mov64(p, 9, 3);
    w = p->n;
    add_imm(p, 5, 5, 1);
    subs1(p, 9);
    k = p->n;
    put(p, 0x54000001u); /* B.NE back */
    patch(p, k, w);
    p->m.pc = at(w);
    for (unsigned turn = 0; turn < 2; ++turn) {
        expect(p, EV_POLL, at(w)); /* the backward branch's */
        mutate(&p->m, 0x30 + 2 * turn + 1, p->poll_keep);
        expect(p, EV_POLL, at(w)); /* the dispatch block's, entered at the loop's head */
        mutate(&p->m, 0x30 + 2 * turn + 2, p->poll_keep);
        p->m.x[5] += 1; /* the next turn */
        subs1_model(p, 9);
    }
    svc(p, 13);
    use_all(p, 1u << 9);
    svc(p, 14);
    ret(p, LR);
    check(p, portable);
}

int main(int argc, char** argv) {
    int portable = argc > 1 && !strcmp(argv[1], "--portable");
    if (argc > 2 || (argc == 2 && !portable)) {
        fprintf(stderr, "usage: snx-test-llvm-promote-sync [--portable]\n");
        return 2;
    }
    cases(portable);
    printf("register promotion at every boundary%s: %d cases, %s\n", portable ? " (portable)" : "",
           g_promote_sync.cases, g_promote_sync.failures ? "FAILED" : "passed");
    return g_promote_sync.failures != 0;
}
