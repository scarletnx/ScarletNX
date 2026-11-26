/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native back end's code on Windows unwinds, x86-64's or with SNX_TEST_ARM64WIN arm64's: functions it made (a leaf, one that calls, one whose registers are
   pinned with the vector ones, a frame of more than a page, an undefined word, a loop's poll, a jump and calls through
   the dispatch table, one of them no function's) are loaded as the runtime JIT's are, their function table registered
   with RtlAddFunctionTable, and run. From the runtime's functions they call RtlLookupFunctionEntry and
   RtlVirtualUnwind, the system's own, unwind the stack to the test's entry, where every register a callee keeps must be
   what the entry marked, and a longjmp out of one crosses their frames. x86-64's run one instruction at a time (the trap
   flag), each unwound the same way, in the middle of a prologue and of an epilogue too. arm64's are unwound from each
   instruction of theirs in a made-up state of it, the one its prologue (or the epilogue it is in) leaves.

   On another host it is the back end's code for Windows that runs, and an unwinder that does what RtlVirtualUnwind does
   with the same table: support/host_win64.c's on x86-64, support/arm64_unwinder.h's under Unicorn; there is no longjmp */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#if defined(SNX_TEST_ARM64WIN)
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "support/arm64_unwinder.h"
#else
#include "common/cpu/recompiler/x64/x64_codegen.h"
#endif
#include "common/runtime/interface.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "support/host_near.h"
#include "support/host_unicorn.h"
#endif

#define BASE  0x10000u
#define WORDS 2048u
#define BIG   1400u /* the long function's additions: each value is read again far on, so it is kept in the frame */
#if defined(SNX_TEST_ARM64WIN)
#define BIG_ADD  0x4EA08400u
#define BIG_REGS 32u
#else
#define BIG_ADD  0x8B000000u
#define BIG_REGS 29u
#endif
#define STACK (1u << 20)
#define STOP  0u
#define GUEST 0x40000u /* the guest's memory, its stack in the middle */

/* the functions, by their first word */
enum {
    F_LEAF = 0,
    F_CALLS = 4,
    F_DIRECT = 12, /* its one call is another function of the unit's */
    F_PINNED = 16,
    F_UNDEF = 36,
    F_POLL = 40,
    F_JUMP = 48,
    F_FLUSHED = 52,
    F_FAR = 56, /* a leaf whose place in the dispatch table is not the first */
    F_MISS = 58,
    F_BIG = 64
};

static struct {
    uint32_t words[WORDS];
    unsigned char* guest;
    CPU* cpu;
    RtAPI* rt;
    SNX_ATOMIC(GuestFn) * disp;
    unsigned svcs, undefs, polls, misses;
    size_t failures;
#if defined(_WIN32)
    size_t unwound, bad;
    unsigned function;      /* the one that runs */
    unsigned char *lo, *hi; /* the loaded code's bounds, by the functions' addresses */
    jmp_buf out;
    int jump;
#else
    HostUc* uc; /* what runs the code */
#endif
#if defined(SNX_TEST_ARM64WIN)
    uint64_t stack[STACK / 8]; /* the made-up states' (walk) */
    /* records': the object whose function table one_frame reads, its .pdata's and .xdata's places, where its code is */
    const unsigned char* object;
    uint32_t pdata, entries, xdata;
    uint64_t code;
#endif
} g_test;

static void failed(const char* what, unsigned function) {
    ++g_test.failures;
    fprintf(stderr, "FAIL: the function at word %u: %s\n", function, what);
}

/* the helpers the functions' cold paths call (x64_helpers.c) */
#define HELPERS(X) X(snx_x64_fp_binary) X(snx_x64_fp_fma) X(snx_x64_fp_unary) X(snx_x64_fp_minmax) X(snx_x64_fp_compare)
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);

static void program(void) {
    uint32_t* w = g_test.words;
    unsigned k;
    for (k = 0; k < WORDS; ++k) w[k] = 0xD65F03C0u; /* RET */
    w[F_LEAF] = 0x91000400u;                        /* ADD X0, X0, #1 */
    {
        /* STP X29, X30, [SP, #-16]!; BL leaf; ADR X9, the far leaf; BLR X9; SVC #1; LDP X29, X30, [SP], #16; RET */
        const uint32_t calls[] = {0xA9BF7BFDu,
                                  0x94000000u | ((unsigned)(F_LEAF - (F_CALLS + 1)) & 0x3FFFFFFu),
                                  0x10000000u | (unsigned)(F_FAR - (F_CALLS + 2)) << 5 | 9u,
                                  0xD63F0120u,
                                  0xD4000021u,
                                  0xA8C17BFDu,
                                  0xD65F03C0u};
        memcpy(w + F_CALLS, calls, sizeof calls);
    }
    {
        /* STP X29, X30, [SP, #-16]!; BL leaf; LDP X29, X30, [SP], #16; RET */
        const uint32_t direct[] = {0xA9BF7BFDu, 0x94000000u | ((unsigned)(F_LEAF - (F_DIRECT + 1)) & 0x3FFFFFFu),
                                   0xA8C17BFDu, 0xD65F03C0u};
        memcpy(w + F_DIRECT, direct, sizeof direct);
    }
    {
        /* MOVZ X16, #3; a loop of scalar floating point, whose registers it keeps in host ones; SVC #2 in it; RET */
        const uint32_t pinned[] = {0xD2800070u, 0x1E210800u, 0x1F010802u, 0x1E232843u, 0x1E653884u,
                                   0x1E61C086u, 0x1E274867u, 0x1E2020E0u, 0x9A83B041u, 0xD4000041u,
                                   0x91000442u, 0x8B020063u, 0xF1000610u, 0x54FFFE81u, 0xD65F03C0u};
        memcpy(w + F_PINNED, pinned, sizeof pinned);
    }
    w[F_UNDEF] = 0x91000400u, w[F_UNDEF + 1] = 0x00000000u; /* ADD, UDF */
    /* MOVZ X0, #3; SUBS X0, X0, #1; B.NE back to it; RET: the poll at the branch */
    w[F_POLL] = 0xD2800060u, w[F_POLL + 1] = 0xF1000400u, w[F_POLL + 2] = 0x54FFFFE1u;
    /* ADR X9, the far leaf; BR X9 */
    w[F_JUMP] = 0x10000000u | (unsigned)(F_FAR - F_JUMP) << 5 | 9u, w[F_JUMP + 1] = 0xD61F0120u;
    w[F_FAR] = 0x91000400u; /* ADD X0, X0, #1 */
    /* ADR X9, two bytes into the far leaf; BLR X9, its one call: no function's, the runtime's miss */
    w[F_MISS] = 0x50000000u | ((unsigned)(F_FAR - F_MISS) & 0x7FFFFu) << 5 | 9u, w[F_MISS + 1] = 0xD63F0120u;
    /* FADD S0, S1, S2, FMUL S3, S0, S1 of denormals FPCR.FZ flushes: the cold paths and their helper */
    w[F_FLUSHED] = 0x1E222820u, w[F_FLUSHED + 1] = 0x1E210803u;
    /* ADD Xd, Xd, Xn (arm64, whose spilled general registers go to vector ones first: ADD Vd.4S, Vd.4S, Vn.4S), d a step
       of the registers and n one far from it, then SVC #3 with the frame up */
    for (k = 0; k < BIG; ++k) {
        unsigned d = (k * 7) % BIG_REGS, n = (k * 11 + 13) % BIG_REGS;
        w[F_BIG + k] = BIG_ADD | n << 16 | d << 5 | d;
    }
    w[F_BIG + BIG] = 0xD4000061u;
}

/* ---- the host: how the code is entered, watched and what it calls ---- */

static void on_svc(CPU* c, u32 number);
static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)word;
    ++g_test.undefs;
    c->pc = pc + 4;
}
static void on_poll(CPU* c) {
    ++g_test.polls;
    c->poll = 0;
}
static void on_miss(CPU* c, u64 pc) {
    ++g_test.misses;
    c->pc = pc;
}

#if defined(SNX_TEST_ARM64WIN)
/* the object of codegen's functions as a Windows host's, and whether this host runs it */
static int make_object(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made,
                       unsigned char** object, size_t* size, char* error, size_t error_size) {
    return snx_arm64_object(codegen, 0, codegen->function_count, options,
                            (snx_arm64_available() ? snx_arm64_features() : SNX_ARM64_F_ALL) | SNX_ARM64_F_WIN, made,
                            object, size, error, error_size);
}
/* the code of the function table's entry pc is in: where it begins, its words. 0 where none is */
static uint32_t code_of(uint64_t pc, uint64_t* begin) {
#if defined(_WIN32)
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL);
    if (!f || (f->UnwindData & 3)) return 0;
    *begin = base + f->BeginAddress;
    return arm64_record((const unsigned char*)(uintptr_t)(base + f->UnwindData)).words;
#else
    return hostuc_table_entry(g_test.uc, pc, begin);
#endif
}
/* a frame of the code's, at s->pc in an entry of the table, made its caller's: NULL, or what can't be */
static const char* one_frame(Arm64State* s) {
    for (uint32_t k = g_test.object ? g_test.entries : 0; k-- > 0;) {
        const unsigned char* e = g_test.object + g_test.pdata + 8 * (size_t)k;
        const uint64_t begin = g_test.code + snx_ld_le32(e);
        const unsigned char* record = g_test.object + g_test.xdata + snx_ld_le32(e + 4);
        if (s->pc >= begin && s->pc < begin + 4ull * arm64_record(record).words) return arm64_unwind(record, begin, s);
    }
    if (g_test.object) return "no entry of the function table";
#if defined(_WIN32)
    CONTEXT c = {0};
    DWORD64 image = 0, frame = 0;
    void* data;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(s->pc, &image, NULL);
    if (!f) return "no entry of the function table";
    c.Pc = s->pc, c.Sp = s->sp;
    memcpy(c.X, s->x, sizeof s->x);
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Pc, f, &c, &data, &frame, NULL);
    s->pc = c.Pc, s->sp = c.Sp;
    memcpy(s->x, c.X, sizeof s->x);
    return NULL;
#else
    return hostuc_unwind_frame(g_test.uc, s);
#endif
}
/* a frame's instruction run on s (the registers it saves in *saved): 1, 0 one that is neither a prologue's nor an
   epilogue's */
static int step_frame(uint32_t w, Arm64State* s, uint32_t* saved) {
    int32_t offset = ((int32_t)((w >> 15 & 0x7F) ^ 0x40) - 0x40) * 8;
    uint32_t rt = w & 31, rt2 = w >> 10 & 31;
    uint64_t imm = (uint64_t)(w >> 10 & 0xFFF) << (w >> 22 & 1 ? 12 : 0), v[2];
    if ((w & 0xFFC07FFFu) == 0xA9807BFDu) { /* STP X29, X30, [SP, #offset]! */
        s->sp += (uint64_t)(int64_t)offset;
        memcpy((void*)(uintptr_t)s->sp, &s->x[29], 16);
    } else if ((w & 0xFFC003E0u) == 0xA90003E0u && rt < 30 && rt2 < 31) { /* STP Xt, Xt2, [SP, #offset] */
        v[0] = s->x[rt], v[1] = s->x[rt2];
        *saved |= 1u << rt | 1u << rt2;
        memcpy((void*)(uintptr_t)(s->sp + (uint64_t)(int64_t)offset), v, 16);
    } else if ((w & 0xFFC003E0u) == 0xA94003E0u && rt < 30 && rt2 < 31) { /* LDP Xt, Xt2, [SP, #offset] */
        memcpy(v, (void*)(uintptr_t)(s->sp + (uint64_t)(int64_t)offset), 16);
        s->x[rt] = v[0], s->x[rt2] = v[1];
    } else if ((w & 0xFFC07FFFu) == 0xA8C07BFDu) { /* LDP X29, X30, [SP], #offset */
        memcpy(&s->x[29], (void*)(uintptr_t)s->sp, 16);
        s->sp += (uint64_t)(int64_t)offset;
    } else if (w == 0x910003FDu) /* MOV X29, SP */
        s->x[29] = s->sp;
    else if (w == 0x91002013u) /* ADD X19, X0, #8 */
        s->x[19] = s->x[0] + 8;
    else if ((w & 0xFF8003FFu) == 0xD10003FFu) /* SUB SP, SP, #imm */
        s->sp -= imm;
    else if ((w & 0xFF8003FFu) == 0x910003FFu) /* ADD SP, SP, #imm */
        s->sp += imm;
    else /* a page's probe */
        return w == 0x910003F1u || (w & 0xFF8003FFu) == 0xD10003F0u || w == 0xD1400631u || w == 0x3940023Fu ||
               w == 0xEB10023Fu || w == 0x54FFFFA8u;
    return 1;
}
/* every instruction of the function at word function unwound from the state it is run in, as its frame's instructions
   make it: the entry's registers, its stack pointer and its return come back. The states, *bad those that did not */
static size_t walk(const uint32_t* code, uint32_t words, uint64_t begin, unsigned function, size_t* bad) {
    Arm64State entry = {0}, body, s, u;
    uint32_t k = 0, n, saved = 0;
    size_t states = 0;
    unsigned r;
    entry.sp = (uint64_t)(uintptr_t)(g_test.stack + STACK / 8) & ~15ull;
    entry.low = (uint64_t)(uintptr_t)g_test.stack, entry.high = entry.low + sizeof g_test.stack;
    for (r = 0; r < 31; ++r) entry.x[r] = 0x5A5A000000000000ull + r * 0x0101010101ull;
    s = entry;
    for (int in_body = 0; k < words; ++k) {
        const char* why = NULL;
        if (in_body) {
            s = body;
            /* ADD SP, then LDPs: an epilogue, its states one after the other */
            for (n = k; n + 1 < words && n < k + 12; ++n) {
                uint32_t w = code[n];
                if (n == k ? (w & 0xFF8003FFu) != 0x910003FFu : (w & 0xFFC003E0u) != 0xA94003E0u) break;
            }
            if (n > k && (code[n] & 0xFFC07FFFu) == 0xA8C07BFDu)
                n = n + 1 - k;
            else
                n = 0;
        } else
            n = 0;
        for (uint32_t e = 0; e <= n; ++e) {
            u = s;
            u.pc = begin + 4ull * (k + e);
            why = one_frame(&u);
            if (!why && u.sp != entry.sp) why = "a stack pointer not the caller's";
            if (!why && u.pc != entry.x[30]) why = "a return not the caller's";
            for (r = 19; !why && r < 30; ++r)
                if (u.x[r] != entry.x[r]) why = "a register not the caller's";
            ++states;
            if (why && ++*bad <= 20)
                fprintf(stderr, "the function at word %u: unwound from +%u: %s\n", function, 4 * (k + e), why);
            if (e < n && !step_frame(code[k + e], &s, &saved)) return *bad += 1, states;
        }
        if (n) {
            k += n; /* the return or the jump after it was the last of its states */
            continue;
        }
        if (!in_body) {
            if (!step_frame(code[k], &s, &saved)) return *bad += 1, states;
            if ((code[k] & 0xFF8003FFu) == 0xD10003FFu) { /* SUB SP: the prologue's last */
                body = s;
                /* what the body may change: X0 to X17, what was saved (X19, the pins) */
                for (r = 0; r < 29; ++r)
                    if (r < 18 || (saved >> r & 1)) body.x[r] = 0xB0D1000000000000ull + r;
                in_body = 1;
            }
        }
    }
    return states;
}
/* the section of a COFF object named name: where its bytes are, its *size; 0 where there is none */
static uint32_t section_of(const unsigned char* object, const char* name, uint32_t* size) {
    for (unsigned k = 0, n = snx_ld_le16(object + 2); k < n; ++k) {
        const unsigned char* h = object + 20 + 40 * (size_t)k;
        if (strncmp((const char*)h, name, 8)) continue;
        *size = snx_ld_le32(h + 16);
        return snx_ld_le32(h + 20);
    }
    return 0;
}
/* the code of a frame of frame bytes, words long: its prologue (pairs of X21 on saved too with pins), NOPs, and an
   epilogue at each of the n words at; then its records, unwound from every one of its instructions */
static size_t record_case(unsigned number, uint32_t frame, int pins, uint32_t words, const uint32_t* at, size_t n,
                          size_t* bad) {
    uint32_t* code = (uint32_t*)malloc(4 * (size_t)words);
    uint32_t k = 0, size = 0, next = 0, imm;
    unsigned char* object = NULL;
    size_t object_size = 0, e, states = 0;
    SnxXElf* o = snx_xelf_new();
    const uint32_t sh = frame > 4095;
    if (!code || !o) return *bad += 1, 0;
    imm = (sh ? frame >> 12 : frame) << 10 | sh << 22;
    for (uint32_t w = 0; w < words; ++w) code[w] = 0xD503201Fu; /* NOP */
    code[k++] = 0xA9BA7BFDu;                                    /* STP X29, X30, [SP, #-96]! */
    code[k++] = 0x910003FDu;                                    /* MOV X29, SP */
    code[k++] = 0xA90153F3u;                                    /* STP X19, X20, [SP, #16] */
    if (pins) {
        code[k++] = 0xA9025BF5u; /* STP X21, X22, [SP, #32]; X23, X24, #48; X25, X26, #64; X27, X28, #80 */
        code[k++] = 0xA90363F7u, code[k++] = 0xA9046BF9u, code[k++] = 0xA90573FBu;
    }
    code[k++] = 0x91002013u; /* ADD X19, X0, #8 */
    if (frame > 4095) {      /* the probe */
        code[k++] = 0x910003F1u, code[k++] = 0xD10003F0u | imm, code[k++] = 0xD1400631u, code[k++] = 0x3940023Fu;
        code[k++] = 0xEB10023Fu, code[k++] = 0x54FFFFA8u;
    }
    code[k++] = 0xD10003FFu | imm; /* SUB SP, SP, #frame */
    for (e = 0; e < n; ++e) {
        k = at[e];
        code[k++] = 0x910003FFu | imm; /* ADD SP, SP, #frame */
        if (pins) code[k++] = 0xA9425BF5u, code[k++] = 0xA94363F7u, code[k++] = 0xA9446BF9u, code[k++] = 0xA94573FBu;
        code[k++] = 0xA94153F3u; /* LDP X19, X20, [SP, #16] */
        code[k++] = 0xA8C67BFDu; /* LDP X29, X30, [SP], #96 */
        code[k++] = 0xD65F03C0u; /* RET */
    }
    snx_xelf_set_machine(o, 183);
    snx_xelf_set_coff(o, 1);
    snx_xelf_put(o, SNX_ELF_TEXT, code, 4 * (size_t)words, 16);
    snx_xelf_symbol(o, "F", SNX_ELF_TEXT, 0, 4 * (size_t)words, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    snx_xelf_unwind(o, 0, 4 * (size_t)words, (const unsigned char*)code, 4 * (size_t)words);
    if (snx_xelf_write(o, &object, &object_size))
        *bad += 1, fprintf(stderr, "case %u: no object of it\n", number);
    else {
        g_test.object = object;
        g_test.pdata = section_of(object, ".pdata", &size);
        g_test.entries = size / 8;
        g_test.xdata = section_of(object, ".xdata", &size);
        g_test.code = (uint64_t)(uintptr_t)code;
        for (e = 0; e < g_test.entries; ++e) { /* the pieces, one after the other, to the end */
            const unsigned char* p = object + g_test.pdata + 8 * e;
            if (snx_ld_le32(p) != next || (snx_ld_le32(p + 4) & 3)) *bad += 1;
            next += 4 * arm64_record(object + g_test.xdata + snx_ld_le32(p + 4)).words;
        }
        if (next != 4 * words) *bad += 1, fprintf(stderr, "case %u: the records end at +%x\n", number, next);
        states = walk(code, words, g_test.code, number, bad);
        g_test.object = NULL;
    }
    free(object), free(code);
    snx_xelf_free(o);
    return states;
}
/* the records the object builder writes (x64_elf.c), each form of them: a frame's sizes on both sides of each code's
   reach, the epilogues a record's header holds and one more, a function of more than a record's 1 MiB, its epilogues
   around the place the records meet */
static size_t records(size_t* bad) {
    enum { PIECE = 0x3FFFF, EPILOGUE = 8, MANY = 33 };
    static const uint32_t frames[] = {0, 16, 496, 512, 4080, 4096, 28672, 32768, 0x80000};
    uint32_t at[MANY + 2];
    unsigned number = 0;
    size_t states = 0, n;
    for (size_t f = 0; f < sizeof frames / sizeof *frames; ++f)
        for (int pins = 0; pins < 2; ++pins) {
            at[0] = 40;
            states += record_case(1000 + number++, frames[f], pins, 64, at, 1, bad);
        }
    for (n = 30; n <= MANY; ++n) { /* the header's 31, and past it */
        for (size_t e = 0; e < n; ++e) at[e] = 40 + 3 * EPILOGUE * (uint32_t)e;
        states += record_case(1000 + number++, 64, 1, 40 + 3 * EPILOGUE * MANY + 16, at, n, bad);
    }
    for (uint32_t before = 0; before <= EPILOGUE;
         ++before) { /* the place the records meet, from each of its words on */
        at[0] = 40;
        at[1] = PIECE - before;
        at[2] = PIECE + 100;
        states += record_case(1000 + number++, 128, 1, PIECE + 200, at, 3, bad);
    }
    at[0] = PIECE + 64; /* none in the first record */
    states += record_case(1000 + number++, 128, 1, 2 * PIECE + 64, at, 1, bad);
    return states;
}
#if defined(_WIN32)
/* what entry() marks before the call and the unwinder must find at it: X19 to X29, D8 to D15; the call's stack pointer
   and return address */
struct Marks {
    uint64_t x[11];   /* 0 */
    uint64_t d[8];    /* 88 */
    uint64_t sp, ret; /* 152, 160 */
};
struct Marks snx_test_marks;
static_assert(offsetof(struct Marks, d) == 88 && offsetof(struct Marks, sp) == 152 &&
                  offsetof(struct Marks, ret) == 160,
              "entry's bytes reach the marks at their offsets");
/* entry(fn X0, state X1, pc X2): fn(state, pc) */
[[gnu::naked]] static void entry(GuestFn fn, CPU* c, u64 pc) {
    __asm__("stp x29, x30, [sp, #-176]!\n\tstp x19, x20, [sp, #16]\n\tstp x21, x22, [sp, #32]\n\t"
            "stp x23, x24, [sp, #48]\n\tstp x25, x26, [sp, #64]\n\tstp x27, x28, [sp, #80]\n\t"
            "stp d8, d9, [sp, #96]\n\tstp d10, d11, [sp, #112]\n\tstp d12, d13, [sp, #128]\n\t"
            "stp d14, d15, [sp, #144]\n\t"
            "mov x9, x0\n\tmov x0, x1\n\tmov x1, x2\n\t"
            "adrp x10, snx_test_marks\n\tadd x10, x10, :lo12:snx_test_marks\n\t"
            "ldp x19, x20, [x10]\n\tldp x21, x22, [x10, #16]\n\tldp x23, x24, [x10, #32]\n\t"
            "ldp x25, x26, [x10, #48]\n\tldp x27, x28, [x10, #64]\n\tldr x29, [x10, #80]\n\t"
            "ldp d8, d9, [x10, #88]\n\tldp d10, d11, [x10, #104]\n\tldp d12, d13, [x10, #120]\n\t"
            "ldp d14, d15, [x10, #136]\n\t"
            "mov x11, sp\n\tstr x11, [x10, #152]\n\tadr x11, 1f\n\tstr x11, [x10, #160]\n\t"
            "blr x9\n"
            "1:\n\t"
            "ldp d8, d9, [sp, #96]\n\tldp d10, d11, [sp, #112]\n\tldp d12, d13, [sp, #128]\n\t"
            "ldp d14, d15, [sp, #144]\n\t"
            "ldp x19, x20, [sp, #16]\n\tldp x21, x22, [sp, #32]\n\tldp x23, x24, [sp, #48]\n\t"
            "ldp x25, x26, [sp, #64]\n\tldp x27, x28, [sp, #80]\n\tldp x29, x30, [sp], #176\n\tret");
}
/* the state c unwound by the system to entry's frame: what is wrong with where it came to, NULL for nothing */
static const char* unwinds(CONTEXT c) {
    const struct Marks* m = &snx_test_marks;
    unsigned frames = 0, k;
    ++g_test.unwound;
    while (c.Pc != m->ret) {
        DWORD64 image = 0, frame = 0;
        void* data;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Pc, &image, NULL);
        if (++frames > 64) return "never reached the entry";
        if (c.Sp > m->sp) return "a stack pointer past the entry's";
        if (f)
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Pc, f, &c, &data, &frame, NULL);
        else /* a leaf's */
            c.Pc = c.Lr;
    }
    if (c.Sp != m->sp) return "a stack pointer not the entry's";
    for (k = 0; k < 11; ++k)
        if (c.X[19 + k] != m->x[k]) return "a general-purpose register not its caller's";
    for (k = 0; k < 8; ++k)
        if (c.V[8 + k].Low != m->d[k]) return "a vector register's low half not its caller's";
    return NULL;
}
/* a function the code calls: the stack from here, through the code's frames, to the entry; or out, across them */
static void on_svc(CPU* c, u32 number) {
    const char* why;
    CONTEXT here;
    ++g_test.svcs;
    (void)c, (void)number;
    if (g_test.jump) longjmp(g_test.out, 7);
    RtlCaptureContext(&here);
    if ((why = unwinds(here)) && ++g_test.bad <= 20)
        fprintf(stderr, "unwound from a call of the function at word %u: %s\n", g_test.function, why);
}
#endif
#else
static int make_object(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made,
                       unsigned char** object, size_t* size, char* error, size_t error_size) {
    return snx_x64_object(codegen, 0, codegen->function_count, options, made, object, size, error, error_size);
}
#endif
#if defined(_WIN32) && !defined(SNX_TEST_ARM64WIN)
/* what entry() marks before the call and the unwinder must find at it: RBX, RBP, RSI, RDI, R12 to R15, XMM6 to XMM15;
   the call's stack pointer and return address */
struct Marks {
    uint64_t mark[8];      /* 0 */
    uint64_t xmark[10][2]; /* 64 */
    uint64_t sp, ret;      /* 224, 232 */
    uint64_t step;         /* 240: the trap flag through the call */
};
struct Marks snx_test_marks;
static_assert(offsetof(struct Marks, xmark) == 64 && offsetof(struct Marks, sp) == 224 &&
                  offsetof(struct Marks, step) == 240,
              "entry's bytes reach the marks at their offsets");
/* entry(fn RCX, state RDX, pc R8): fn(state, pc) */
[[gnu::naked]] static void entry(GuestFn fn, CPU* c, u64 pc) {
    __asm__(
        "pushq %rbx\n\tpushq %rbp\n\tpushq %rsi\n\tpushq %rdi\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
        "subq $200, %rsp\n\t"
        "movups %xmm6, 32(%rsp)\n\tmovups %xmm7, 48(%rsp)\n\tmovups %xmm8, 64(%rsp)\n\tmovups %xmm9, 80(%rsp)\n\t"
        "movups %xmm10, 96(%rsp)\n\tmovups %xmm11, 112(%rsp)\n\tmovups %xmm12, 128(%rsp)\n\t"
        "movups %xmm13, 144(%rsp)\n\tmovups %xmm14, 160(%rsp)\n\tmovups %xmm15, 176(%rsp)\n\t"
        "movq %rcx, %rax\n\tmovq %rdx, %rcx\n\tmovq %r8, %rdx\n\t"
        "movq snx_test_marks+0(%rip), %rbx\n\tmovq snx_test_marks+8(%rip), %rbp\n\t"
        "movq snx_test_marks+16(%rip), %rsi\n\tmovq snx_test_marks+24(%rip), %rdi\n\t"
        "movq snx_test_marks+32(%rip), %r12\n\tmovq snx_test_marks+40(%rip), %r13\n\t"
        "movq snx_test_marks+48(%rip), %r14\n\tmovq snx_test_marks+56(%rip), %r15\n\t"
        "movups snx_test_marks+64(%rip), %xmm6\n\tmovups snx_test_marks+80(%rip), %xmm7\n\t"
        "movups snx_test_marks+96(%rip), %xmm8\n\tmovups snx_test_marks+112(%rip), %xmm9\n\t"
        "movups snx_test_marks+128(%rip), %xmm10\n\tmovups snx_test_marks+144(%rip), %xmm11\n\t"
        "movups snx_test_marks+160(%rip), %xmm12\n\tmovups snx_test_marks+176(%rip), %xmm13\n\t"
        "movups snx_test_marks+192(%rip), %xmm14\n\tmovups snx_test_marks+208(%rip), %xmm15\n\t"
        "movq %rsp, snx_test_marks+224(%rip)\n\t"
        "leaq 2f(%rip), %r8\n\tmovq %r8, snx_test_marks+232(%rip)\n\t"
        "cmpq $0, snx_test_marks+240(%rip)\n\tje 1f\n\t"
        "pushfq\n\torq $0x100, (%rsp)\n\tpopfq\n"
        "1:\n\tcallq *%rax\n"
        "2:\n\tmovq $0, snx_test_marks+240(%rip)\n\t" /* the next step is the last */
        "movups 32(%rsp), %xmm6\n\tmovups 48(%rsp), %xmm7\n\tmovups 64(%rsp), %xmm8\n\tmovups 80(%rsp), %xmm9\n\t"
        "movups 96(%rsp), %xmm10\n\tmovups 112(%rsp), %xmm11\n\tmovups 128(%rsp), %xmm12\n\t"
        "movups 144(%rsp), %xmm13\n\tmovups 160(%rsp), %xmm14\n\tmovups 176(%rsp), %xmm15\n\t"
        "addq $200, %rsp\n\t"
        "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rdi\n\tpopq %rsi\n\tpopq %rbp\n\tpopq %rbx\n\tretq");
}
/* the state c unwound by the system to entry's frame: what is wrong with where it came to, NULL for nothing */
static const char* unwinds(CONTEXT c) {
    const struct Marks* m = &snx_test_marks;
    unsigned frames = 0, k;
    ++g_test.unwound;
    while (c.Rip != m->ret) {
        DWORD64 image = 0, frame = 0;
        void* data;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (++frames > 64) return "never reached the entry";
        if (c.Rsp > m->sp) return "a stack pointer past the entry's";
        if (f)
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &data, &frame, NULL);
        else { /* a leaf's */
            c.Rip = *(DWORD64*)(uintptr_t)c.Rsp;
            c.Rsp += 8;
        }
    }
    if (c.Rsp != m->sp) return "a stack pointer not the entry's";
    {
        const uint64_t got[8] = {c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15};
        for (k = 0; k < 8; ++k)
            if (got[k] != m->mark[k]) return "a general-purpose register not its caller's";
    }
    for (k = 0; k < 10; ++k)
        if (memcmp(&(&c.Xmm6)[k], m->xmark[k], 16)) return "an XMM register not its caller's";
    return NULL;
}
static void unwound(const CONTEXT* c, const char* where) {
    const char* why = unwinds(*c);
    if (!why) return;
    if (++g_test.bad <= 20)
        fprintf(stderr, "unwound from %s, +%llx of the code (the function at word %u): %s\n", where,
                (unsigned long long)(c->Rip - (DWORD64)(uintptr_t)g_test.lo), g_test.function, why);
}
static LONG WINAPI on_step(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (e->ContextRecord->Rip >= (DWORD64)(uintptr_t)g_test.lo && e->ContextRecord->Rip < (DWORD64)(uintptr_t)g_test.hi)
        unwound(e->ContextRecord, "an instruction");
    if (snx_test_marks.step) e->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* a function the code calls: the stack from here, through the code's frames, to the entry; or out, across them */
static void on_svc(CPU* c, u32 number) {
    ++g_test.svcs;
    (void)c, (void)number;
    if (g_test.jump) longjmp(g_test.out, 7);
    CONTEXT here;
    RtlCaptureContext(&here);
    unwound(&here, "a call");
}
#endif
#if !defined(_WIN32)
static void on_svc(CPU* c, u32 number) {
    (void)c, (void)number;
    ++g_test.svcs;
}
#endif

/* ---- the runs ---- */

static void start(unsigned function) {
    CPU* c = g_test.cpu;
    unsigned k;
    memset(c, 0, sizeof *c);
    for (k = 0; k < 31; ++k) c->x[k] = 0x0101010101010101ull * k;
    for (k = 0; k < 32; ++k) c->v[k].d[0] = 0x3F8000003F800000ull + k, c->v[k].d[1] = k;
    if (function == F_FLUSHED) {
        c->fpcr = 1u << 24;                                     /* FZ */
        c->v[1].d[0] = 0x00000001u, c->v[2].d[0] = 0x00000002u; /* denormals */
    }
    c->x[31] = GUEST / 2; /* SP */
    c->x[30] = STOP;
    c->mem = g_test.guest;
    c->rt = g_test.rt;
    c->disp = g_test.disp;
    c->code_lo = BASE;
    c->code_hi = BASE + sizeof g_test.words;
    c->poll = function == F_POLL;
    c->pc = BASE + 4 * function;
}
/* what the function at word function must have done: its state's, as the words say */
static void check(unsigned function) {
    const CPU* c = g_test.cpu;
    switch (function) {
        case F_LEAF:
        case F_FAR:
        case F_DIRECT:
        case F_JUMP:
            if (c->x[0] != 1) failed("X0 not the leaf's", function);
            break;
        case F_MISS:
            if (g_test.misses != 1 || c->pc != BASE + 4 * F_FAR + 2 || c->x[0])
                failed("its call into an instruction not the runtime's miss", function);
            break;
        case F_CALLS:
            if (c->x[0] != 2 || !g_test.svcs) failed("its calls not made", function);
            break;
        case F_PINNED:
            if (g_test.svcs != 3 || c->x[16]) failed("its loop not run three times", function);
            break;
        case F_UNDEF:
            if (g_test.undefs != 1) failed("the undefined word not raised once", function);
            break;
        case F_POLL:
            if (g_test.polls != 1 || c->x[0]) failed("its loop or its poll not run", function);
            break;
        case F_FLUSHED:
            if (!(c->fpsr & 0x80)) failed("the denormals not flushed (FPSR.IDC)", function);
            break;
        default: {
            unsigned k, bad = 0;
#if defined(SNX_TEST_ARM64WIN)
            uint32_t v[BIG_REGS][4];
            for (k = 0; k < BIG_REGS; ++k) {
                const uint64_t d[2] = {0x3F8000003F800000ull + k, k}; /* start's */
                memcpy(v[k], d, sizeof d);
            }
            for (k = 0; k < BIG; ++k)
                for (unsigned e = 0; e < 4; ++e) v[(k * 7) % BIG_REGS][e] += v[(k * 11 + 13) % BIG_REGS][e];
            for (k = 0; k < BIG_REGS; ++k) bad += memcmp(&c->v[k], v[k], 16) != 0;
#else
            uint64_t x[BIG_REGS];
            for (k = 0; k < BIG_REGS; ++k) x[k] = 0x0101010101010101ull * k;
            for (k = 0; k < BIG; ++k) x[(k * 7) % BIG_REGS] += x[(k * 11 + 13) % BIG_REGS];
            for (k = 0; k < BIG_REGS; ++k) bad += c->x[k] != x[k];
#endif
            if (bad || g_test.svcs != 1) failed("its sums", function);
        }
    }
}

#if !defined(_WIN32)
/* the pages the code's 32-bit distances reach, where the ones next to its arena are taken (as a sanitizer's layout may
   have them): those of the one free place above the arena, then of the one below it, though the system's own choice is
   a free place past the code's reach, higher than both */
static int check_near(void) {
    const size_t span = (size_t)4 << 30, arena = (size_t)256 << 20, hole = (size_t)4 << 20, page = (size_t)1 << 20;
    unsigned char* r = (unsigned char*)mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    int bad = 0;
    if (r == MAP_FAILED) {
        fprintf(stderr, "FAIL: no address space of %zu bytes\n", span);
        return 1;
    }
    unsigned char* base = r + ((size_t)5 << 28);
    unsigned char* far = r + span - 2 * hole;
    munmap(far, hole);
    for (int below = 0; below < 2; ++below) {
        const char* where = below ? "below" : "above";
        unsigned char* at = below ? base - ((size_t)300 << 20) : base + arena + ((size_t)100 << 20);
        munmap(at, hole);
        unsigned char* p = (unsigned char*)hostuc_near(base, arena, page);
        if (!p)
            ++bad, fprintf(stderr, "FAIL: no pages found, with a free place %s the arena\n", where);
        else if (p < at || p + page > at + hole)
            ++bad, fprintf(stderr, "FAIL: the pages are %lld MiB from the arena, not in the free place %s it\n",
                           (long long)((intptr_t)p - (intptr_t)base) / (1 << 20), where);
        if (p) munmap(p, page);
        if (mmap(at, hole, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
            ++bad, fprintf(stderr, "FAIL: the free place %s the arena not taken back\n", where);
    }
    munmap(r, span - 2 * hole), munmap(far + hole, hole); /* what the process mapped in the far place meanwhile stays */
    return bad;
}
#endif
#if defined(SNX_TEST_ARM64WIN) && !defined(_WIN32)
/* a function so far in its module that its stale byte is past what COFF's ADRP holds: its entry reads that byte, and no
   other. The bytes start one past a page's start, and the function's is the first of a page, so each bit of the ADRP's
   addend counts */
static void stale_far(const SnxCodegen* codegen) {
    enum { FAR = (1u << 20) + 0x2FFF };
    SnxCodegen cg = *codegen;
    SnxNativeOptions options = {.stale = 1};
    unsigned char made = 0, *object = NULL, *stale;
    size_t size = 0, k;
    char error[512] = "", sym[40];
    uint64_t fn = 0;
    CPU* c;
    HostUc* uc = hostuc_new(HOSTUC_ARM64_WIN, 1u << 16);
    const SnxFunction* leaf = snx_codegen_function(codegen, BASE + 4 * F_LEAF);
    cg.functions = (SnxFunction*)calloc(FAR + 1, sizeof *cg.functions);
    cg.function_count = FAR + 1;
    stale = uc ? (unsigned char*)hostuc_alloc(uc, FAR + 2) : NULL;
    c = uc ? (CPU*)hostuc_alloc(uc, sizeof *c) : NULL;
    if (!cg.functions || !leaf || !stale || !c) {
        failed("no room for a module of a million functions", F_LEAF);
        free(cg.functions);
        hostuc_free(uc);
        return;
    }
    ++stale;
    cg.functions[FAR] = *leaf;
    snprintf(sym, sizeof sym, "F_%llX", (unsigned long long)leaf->start);
    for (k = 0; k < FAR; ++k) stale[k] = 1;
    if (snx_arm64_object(&cg, FAR, FAR + 1, &options, SNX_ARM64_F_ALL | SNX_ARM64_F_WIN, &made, &object, &size, error,
                         sizeof error) < 0 ||
        !object || hostuc_define(uc, "snx_stale", (uint64_t)(uintptr_t)stale) ||
        hostuc_load(uc, object, size, error, sizeof error) || !(fn = hostuc_symbol(uc, sym)))
        failed(error[0] ? error : "no object of the far function", F_LEAF);
    else
        for (k = 0; k < 2; ++k) {
            stale[FAR] = (unsigned char)k;
            memset(c, 0, sizeof *c);
            c->x[30] = STOP;
            if (hostuc_call(uc, fn, (uint64_t)(uintptr_t)c, leaf->start, NULL) || c->x[0] != !k ||
                c->pc != (k ? leaf->start : STOP))
                failed(k ? "its stale byte not read" : "another function's stale byte read", F_LEAF);
        }
    free(object), free(cg.functions);
    hostuc_free(uc);
}
#endif

/* two JIT units of the same functions load beside each other: a unit's functions are its own, its table its one global */
static SnxU64 any_symbol(void* user, const char* name) {
    (void)user, (void)name;
    return (SnxU64)(uintptr_t)&g_test;
}
static void two_units(SnxCodegen* codegen) {
    static const char* const tables[2] = {"UNIT_A_TABLE", "UNIT_B_TABLE"};
    char error[512] = "";
    SnxCodeLoader* loader =
#if defined(SNX_TEST_ARM64WIN)
        snx_code_loader_new_machine((size_t)16 << 20, SNX_CODE_COFF_ARM64, error, sizeof error);
#else
        snx_code_loader_new_machine((size_t)16 << 20, SNX_CODE_COFF_X64, error, sizeof error);
#endif
    unsigned char* made = (unsigned char*)calloc(codegen->function_count + 1, 1);
    SnxU64 at = 0, got[2] = {0};
    if (!loader || !made) {
        failed("no loader for two units", 0);
    } else {
        snx_code_set_resolver(loader, any_symbol, NULL);
        for (int k = 0; k < 2; ++k) {
            SnxNativeOptions options = {.jit = 1, .table_name = tables[k]};
            unsigned char* object = NULL;
            size_t size = 0;
            SnxLinkInput input = {NULL, 0, "unit"};
            if (make_object(codegen, &options, made, &object, &size, error, sizeof error) < 0 || !object)
                failed("no object of a unit", 0);
            else {
                input.data = object;
                input.size = size;
                if (snx_code_load(loader, &input, 1, &tables[k], 1, &at, error, sizeof error))
                    failed("the second unit of the same functions", 0), fprintf(stderr, "%s\n", error);
                else
                    got[k] = at;
            }
            free(object);
        }
        if (!got[0] || !got[1] || got[0] == got[1]) failed("two units' tables", 0);
    }
    free(made);
    snx_code_loader_free(loader);
}

int main(void) {
    static const unsigned functions[] = {F_LEAF,    F_CALLS, F_PINNED, F_UNDEF,  F_POLL, F_JUMP,
                                         F_FLUSHED, F_FAR,   F_MISS,   F_DIRECT, F_BIG};
    enum { COUNT = sizeof functions / sizeof *functions };
    SnxCodegen codegen;
    SnxCodeImage image = {0};
    SnxNativeOptions options = {.jit = 1, .table_name = "UNWIND_TABLE"};
    SnxU64 hints[COUNT];
    unsigned char *made, *object = NULL;
    const uint64_t* table;
    size_t size = 0, unwound_count = 0, bad = 0, k;
    char error[512] = "";
#if defined(SNX_TEST_ARM64WIN)
    const char* name = "arm64";
#if defined(_WIN32)
    const int available = snx_arm64_available();
#else
    const int available = 1; /* under Unicorn */
#endif
#else
    const char* name = "x64";
    const int available = snx_x64_available();
#endif
    if (!available) {
        printf("%s unwind: no back end of it on this host\n", name);
        return 77;
    }
    program();
#if !defined(_WIN32)
    if (check_near()) ++g_test.failures;
#if defined(SNX_TEST_ARM64WIN)
    HostUc* uc = g_test.uc = hostuc_new(HOSTUC_ARM64_WIN, 1u << 16);
#else
    snx_x64_set_win64(1);
    HostUc* uc = hostuc_new(HOSTUC_WIN64, 1u << 16);
#endif
    if (!uc) {
#if defined(SNX_TEST_ARM64WIN)
        printf("SKIP: no Unicorn arm64 to run its code\n");
        return 77;
#else
        fprintf(stderr, "FAIL: no loader for the code\n");
        return 1;
#endif
    }
    g_test.cpu = (CPU*)hostuc_alloc(uc, sizeof(CPU));
    g_test.rt = (RtAPI*)hostuc_alloc(uc, sizeof(RtAPI));
#define NATIVE(f) hostuc_native(uc, #f, (void*)f)
#else
    SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof error);
    if (!loader) {
        fprintf(stderr, "FAIL: no loader for the code: %s\n", error);
        return 1;
    }
    g_test.cpu = (CPU*)calloc(1, sizeof(CPU));
    g_test.rt = (RtAPI*)calloc(1, sizeof(RtAPI));
#define NATIVE(f) ((uint64_t)(uintptr_t)f)
#endif
#if defined(SNX_TEST_ARM64WIN) && !defined(_WIN32)
    g_test.guest = (unsigned char*)hostuc_alloc(uc, GUEST); /* where Unicorn has them too */
    g_test.disp = (SNX_ATOMIC(GuestFn)*)hostuc_alloc(uc, WORDS * sizeof *g_test.disp);
#else
    g_test.guest = (unsigned char*)calloc(1, GUEST);
    g_test.disp = (SNX_ATOMIC(GuestFn)*)calloc(WORDS, sizeof *g_test.disp);
#endif
    if (!g_test.cpu || !g_test.rt || !g_test.guest || !g_test.disp) {
        fprintf(stderr, "FAIL: out of memory\n");
        return 1;
    }
    g_test.rt->svc = (void (*)(CPU*, u32))(uintptr_t)NATIVE(on_svc);
    g_test.rt->undef = (void (*)(CPU*, u64, u32))(uintptr_t)NATIVE(on_undef);
    g_test.rt->poll = (void (*)(CPU*))(uintptr_t)NATIVE(on_poll);
    g_test.rt->miss = (GuestFn)(uintptr_t)NATIVE(on_miss);
    image.image = (unsigned char*)g_test.words;
    image.image_size = image.text_size = sizeof g_test.words;
    image.base = BASE;
    for (k = 0; k < COUNT; ++k) hints[k] = BASE + 4 * functions[k];
    if (snx_codegen_analyze(&codegen, &image, 1, hints, COUNT, 1, SNX_A64_TARGET_SWITCH)) {
        fprintf(stderr, "FAIL: no functions found\n");
        return 1;
    }
    made = (unsigned char*)calloc(codegen.function_count + 1, 1);
    if (!made || make_object(&codegen, &options, made, &object, &size, error, sizeof error) < 0 || !object) {
        fprintf(stderr, "FAIL: the back end made no unit of the functions: %s\n", error);
        return 1;
    }
#if defined(_WIN32)
    {
        SnxLinkInput input = {object, size, "unwind"};
        const char* name = "UNWIND_TABLE";
        SnxU64 at = 0;
#define DEFINE(f) snx_code_define(loader, #f, NATIVE(f));
        HELPERS(DEFINE)
        if (snx_code_load(loader, &input, 1, &name, 1, &at, error, sizeof error)) {
            fprintf(stderr, "FAIL: loading the unit: %s\n", error);
            return 1;
        }
        table = (const uint64_t*)(uintptr_t)at;
    }
#else
#define DEFINE(f) NATIVE(f);
    HELPERS(DEFINE)
    if (hostuc_load(uc, object, size, error, sizeof error)) {
        fprintf(stderr, "FAIL: loading the unit: %s\n", error);
        return 1;
    }
    table = (const uint64_t*)(uintptr_t)hostuc_symbol(uc, "UNWIND_TABLE");
#endif
    for (k = 0; k < codegen.function_count; ++k) {
        GuestFn fn = (GuestFn)(uintptr_t)table[k];
        atomic_store_explicit(&g_test.disp[(codegen.functions[k].start - BASE) / 4], fn, memory_order_relaxed);
#if defined(_WIN32) && !defined(SNX_TEST_ARM64WIN)
        if (!g_test.lo || (unsigned char*)(uintptr_t)table[k] < g_test.lo)
            g_test.lo = (unsigned char*)(uintptr_t)table[k];
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(table[k], &base, NULL);
            if (!f) {
                fprintf(stderr, "FAIL: RtlLookupFunctionEntry finds no entry for the function at word %llu\n",
                        (unsigned long long)(codegen.functions[k].start - BASE) / 4);
                return 1;
            }
            if (g_test.hi < (unsigned char*)(uintptr_t)(base + f->EndAddress))
                g_test.hi = (unsigned char*)(uintptr_t)(base + f->EndAddress);
        }
#endif
    }
#if defined(_WIN32) && defined(SNX_TEST_ARM64WIN)
    for (k = 0; k < 11; ++k) snx_test_marks.x[k] = 0x5151000000000000ull + k * 0x0101010101ull;
    for (k = 0; k < 8; ++k) snx_test_marks.d[k] = 0x6262000000000000ull + k * 0x0303030303ull;
#elif defined(_WIN32)
    AddVectoredExceptionHandler(1, on_step);
    for (k = 0; k < 8; ++k) snx_test_marks.mark[k] = 0x5151000000000000ull + k * 0x0101010101ull;
    for (k = 0; k < 10; ++k)
        snx_test_marks.xmark[k][0] = 0x6262000000000000ull + k * 0x0303030303ull,
        snx_test_marks.xmark[k][1] = 0x7373000000000000ull + k * 0x0505050505ull;
#else
    hostuc_step(uc, 1);
#endif
    for (k = 0; k < COUNT; ++k) {
        unsigned f = functions[k];
        GuestFn fn = atomic_load_explicit(&g_test.disp[f], memory_order_relaxed);
        size_t before = g_test.failures;
        if (!fn) {
            failed("no code of it", f);
            continue;
        }
        g_test.svcs = g_test.undefs = g_test.polls = g_test.misses = 0;
        start(f);
#if defined(_WIN32)
        g_test.function = f;
#if !defined(SNX_TEST_ARM64WIN)
        snx_test_marks.step = 1;
#endif
        entry(fn, g_test.cpu, g_test.cpu->pc);
#if !defined(SNX_TEST_ARM64WIN)
        snx_test_marks.step = 0;
#endif
#else
        if (hostuc_call(uc, (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)g_test.cpu, g_test.cpu->pc, NULL))
            failed("its registers or its unwinding", f);
#endif
        if (g_test.failures == before) check(f);
    }
#if defined(_WIN32)
    unwound_count = g_test.unwound, bad = g_test.bad;
    /* out of a call the code made, across its frames: the one that calls, the pinned one, the long one */
    {
        static const unsigned out[] = {F_CALLS, F_PINNED, F_BIG};
        volatile unsigned n;
        g_test.jump = 1;
        for (n = 0; n < sizeof out / sizeof *out; ++n) switch (setjmp(g_test.out)) {
                case 0:
                    start(out[n]);
                    atomic_load_explicit(&g_test.disp[out[n]], memory_order_relaxed)(g_test.cpu, g_test.cpu->pc);
                    failed("returned rather than jumped out of", out[n]);
                    break;
                case 7: break;
                default: failed("the longjmp out of it came back wrong", out[n]);
            }
        g_test.jump = 0;
    }
#else
    unwound_count = hostuc_unwound(uc, &bad);
#endif
#if defined(SNX_TEST_ARM64WIN)
    unwound_count += records(&bad);
    /* and from each instruction of each function */
    for (k = 0; k < codegen.function_count; ++k) {
        const unsigned f = (unsigned)((codegen.functions[k].start - BASE) / 4);
        uint64_t begin = 0;
        uint32_t words = code_of(table[k], &begin);
        if (!words || begin != table[k])
            failed("no entry of the function table", f);
        else
            unwound_count += walk((const uint32_t*)(uintptr_t)begin, words, begin, f, &bad);
    }
#endif
#if defined(SNX_TEST_ARM64WIN) && !defined(_WIN32)
    stale_far(&codegen);
#endif
    two_units(&codegen);
    if (bad) ++g_test.failures;
#if !defined(SNX_TEST_ARM64WIN)
    {
        /* a callee of the object's may be another compiler's, which has 32 bytes there to write (in an AOT module LLVM's):
           push rbx; mov rbx, rcx; sub rsp, imm32 */
        const unsigned char* p =
            (const unsigned char*)(uintptr_t)atomic_load_explicit(&g_test.disp[F_DIRECT], memory_order_relaxed);
        if (!p || memcmp(p, "\x53\x48\x89\xCB\x48\x81\xEC", 7) || p[7] < 32)
            failed("its frame has no room for its callee's", F_DIRECT);
    }
#endif
    {
        /* the long function's frame is past a page: its prologue reads each page first */
#if defined(SNX_TEST_ARM64WIN)
        /* LDRB WZR, [X17]; CMP X17, X16 */
        static const unsigned char probe[] = {0x3F, 0x02, 0x40, 0x39, 0x3F, 0x02, 0x10, 0xEB};
#else
        /* mov r9, rsp; sub r9, 4096 */
        static const unsigned char probe[] = {0x49, 0x89, 0xE1, 0x49, 0x81, 0xE9, 0x00, 0x10, 0x00, 0x00};
#endif
        size_t probes = 0;
        for (k = 0; k + sizeof probe <= size; ++k) probes += !memcmp(object + k, probe, sizeof probe);
        if (probes != 1)
            ++g_test.failures, fprintf(stderr, "FAIL: %zu frames of a page or more, not the long one's\n", probes);
    }
    if (unwound_count < BIG) ++g_test.failures, fprintf(stderr, "FAIL: only %zu states unwound\n", unwound_count);
    printf("%s unwind: %zu functions, %zu bytes; %zu states unwound, %zu wrongly\n", name, codegen.function_count, size,
           unwound_count, bad);
    free(made), free(object);
    snx_codegen_free(&codegen);
#if !defined(_WIN32)
    hostuc_free(uc);
#endif
    return g_test.failures != 0;
}
