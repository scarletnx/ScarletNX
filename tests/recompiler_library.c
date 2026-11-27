/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler as a library (snx_recompiler.h): hand-assembled A64 and A32 programs in a flat guest memory,
   recompiled in this process and run against the SVC, BRK and undefined-instruction
   hooks a caller supplies. No game data, no files but the optional IR dump. Arguments: --verbose for the recompiler's
   progress lines. */
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* ---- the A64 program (llvm-mc -triple=aarch64), at CODE64 in guest memory ----
   entry:  mov x0,#0; mov x1,#10; mov x3,x30          (the link register, which bl overwrites)
   loop:   add x0,x0,x1; sub x1,x1,#1; cbnz x1,loop   (x0 = 55)
           bl helper                                   (x0 = 1055)
           svc #0x42                                   (the handler checks x0 and sets it to 7)
           str x0,[x2]; mov x30,x3; ret
   helper: add x0,x0,#1000; ret
   far:    mov x0,#99; brk #7; ret                     (reached by no call: a hint, or the JIT)
   ivau:   ic ivau,x0; ret                             (reached by no call: RtAPI.code_changed gets x0's line) */
#define CODE64 0x1000u
#define FAR    (CODE64 + 13 * 4)
#define IVAU   (CODE64 + 16 * 4)
#define UNDEF  (CODE64 + 20 * 4) /* zero words: UDF #0 */
static const uint32_t a64_words[] = {0xd2800000, 0xd2800141, 0xaa1e03e3, 0x8b010000, 0xd1000421, 0xb5ffffc1,
                                     0x94000005, 0xd4000841, 0xf9000040, 0xaa0303fe, 0xd65f03c0, 0x910fa000,
                                     0xd65f03c0, 0xd2800c60, 0xd42000e0, 0xd65f03c0, 0xd50b7520, 0xd65f03c0};
#define A64_SIZE (sizeof(a64_words))
/* ---- the A32 program (llvm-mc -triple=armv8a), at CODE32: mov r0,#5; add r0,r0,#2; mov r7,#0x11; svc #0; bx lr */
#define CODE32   0x2000u
static const uint32_t a32_words[] = {0xe3a00005, 0xe2800002, 0xe3a07011, 0xef000000, 0xe12fff1e};
#define MEMORY (64u * 1024u)
#define STORE  0x8000u /* str x0,[x2] lands here */
#define STOP   0u      /* x30 of the entry: returning there ends the run */

static void place(uint8_t* mem, uint64_t address, const uint32_t* words, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        uint32_t w = words[i];
        mem[address + i * 4 + 0] = (uint8_t)w;
        mem[address + i * 4 + 1] = (uint8_t)(w >> 8);
        mem[address + i * 4 + 2] = (uint8_t)(w >> 16);
        mem[address + i * 4 + 3] = (uint8_t)(w >> 24);
    }
}

/* the caller's runtime: the hooks record what they saw in the CPU's own context */
typedef struct {
    unsigned svcs, brks, undefs, changes;
    uint32_t svc_imm, brk_imm, undef_word;
    uint64_t svc_x0, brk_pc, undef_pc, changed;
} Seen;
static void svc64(CPU* c, uint32_t imm) {
    Seen* s = (Seen*)c->thread;
    ++s->svcs;
    s->svc_imm = imm;
    s->svc_x0 = c->x[0];
    c->x[0] = 7;
}
static void brk64(CPU* c, uint64_t pc, uint32_t imm) {
    Seen* s = (Seen*)c->thread;
    ++s->brks;
    s->brk_imm = imm;
    s->brk_pc = pc;
    c->pc = STOP; /* ends snx_recompiled_run */
}
static void undef64(CPU* c, uint64_t pc, uint32_t word) {
    Seen* s = (Seen*)c->thread;
    ++s->undefs;
    s->undef_word = word;
    s->undef_pc = pc;
    c->pc = STOP;
}

static void code_changed64(CPU* c, uint64_t line) {
    Seen* s = (Seen*)c->thread;
    ++s->changes;
    s->changed = line;
}

static uint64_t read64(const uint8_t* p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

/* the JIT's cache file: the module's JIT compiles far at its miss and the unit lands in library-jit.cache in the cache
   folder, and a second module of the same code runs far again, now from that file. Without a cache folder, no file */
static void jit_cache(struct snx_recompiler* r, uint8_t* mem, const char* folder) {
    struct snx_recompiled* m;
    char* file = snx_path_join(folder, "library-jit.cache");
    unsigned pass;
    check(file != NULL, "a path for the JIT cache file");
    if (!file) return;
    for (pass = 0; pass < 2; ++pass) {
        Seen seen = {0};
        RtAPI rt;
        CPU cpu = {0};
        FILE* f;
        long size = 0;
        check(snx_recompiler_set_cache(r, folder) == 0, "a cache folder");
        m = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
        check(m != NULL, "jit cache: a module with a cache folder");
        if (!m) break;
        snx_runtime_defaults(&rt);
        rt.svc = svc64;
        rt.undef = undef64;
        rt.brk = brk64;
        cpu.mem = mem;
        cpu.rt = &rt;
        cpu.thread = &seen;
        check(snx_recompiled_bind(m, &cpu) == 0, "jit cache: bind");
        cpu.x[30] = STOP;
        snx_recompiled_run(m, &cpu, FAR, STOP);
        check(seen.brks == 1 && seen.brk_imm == 7 && cpu.x[0] == 99, "jit cache: far compiled by the JIT, run");
        snx_recompiled_free(m); /* stops the JIT's threads: nothing writes the file after this */
        f = fopen(file, "rb");
        if (f) {
            if (!fseek(f, 0, SEEK_END)) size = ftell(f);
            fclose(f);
        }
        check(size > 0,
              pass ? "jit cache: the file is still there for a second module" : "jit cache: the unit was kept");
    }
    check(snx_recompiler_set_cache(r, NULL) == 0, "the cache folder off");
    free(file);
}

/* ---- a switch through a jump table (llvm-mc -triple=aarch64), at SWITCH64: LLVM's compressed table of bytes, its
   cases counted in instructions from case0, the table permuted so case k is body (5k mod 32) ----
   f:      cmp w0, #31; b.hi dflt; mov w8, w0; adr x9, table; adr x10, case0; ldrb w11, [x9, x8];
           add x10, x10, x11, lsl #2; br x10
   casek:  movz x0, #(7k + 3); ret                    (k 0..31: 32 cases, so many leaders the BR takes the table)
   dflt:   movz x0, #999; ret
   table:  .byte (5k mod 32) * 2 for k 0..31 */
#define SWITCH64 0xA000u
static unsigned switch_misses;
static void (*switch_miss_next)(CPU*, uint64_t);
static void switch_miss(CPU* c, uint64_t pc) {
    ++switch_misses;
    switch_miss_next(c, pc);
}
static const uint32_t switch_words[] = {
    0x71007c1f, 0x540008e8, 0x2a0003e8, 0x100008e9, 0x1000008a, 0x3868692b,
    0x8b0b094a, 0xd61f0140, 0xd2800060, 0xd65f03c0, 0xd2800140, 0xd65f03c0,
    0xd2800220, 0xd65f03c0, 0xd2800300, 0xd65f03c0, 0xd28003e0, 0xd65f03c0,
    0xd28004c0, 0xd65f03c0, 0xd28005a0, 0xd65f03c0, 0xd2800680, 0xd65f03c0,
    0xd2800760, 0xd65f03c0, 0xd2800840, 0xd65f03c0, 0xd2800920, 0xd65f03c0,
    0xd2800a00, 0xd65f03c0, 0xd2800ae0, 0xd65f03c0, 0xd2800bc0, 0xd65f03c0,
    0xd2800ca0, 0xd65f03c0, 0xd2800d80, 0xd65f03c0, 0xd2800e60, 0xd65f03c0,
    0xd2800f40, 0xd65f03c0, 0xd2801020, 0xd65f03c0, 0xd2801100, 0xd65f03c0,
    0xd28011e0, 0xd65f03c0, 0xd28012c0, 0xd65f03c0, 0xd28013a0, 0xd65f03c0,
    0xd2801480, 0xd65f03c0, 0xd2801560, 0xd65f03c0, 0xd2801640, 0xd65f03c0,
    0xd2801720, 0xd65f03c0, 0xd2801800, 0xd65f03c0, 0xd28018e0, 0xd65f03c0,
    0xd28019c0, 0xd65f03c0, 0xd2801aa0, 0xd65f03c0, 0xd2801b80, 0xd65f03c0,
    0xd2807ce0, 0xd65f03c0, 0x1e140a00, 0x063c3228, 0x2e241a10, 0x160c0238,
    0x3e342a20, 0x261c1208, 0x0e043a30, 0x362c2218,
};
static void switch64(struct snx_recompiler* r, uint8_t* mem) {
    int backend;
    place(mem, SWITCH64, switch_words, sizeof(switch_words) / sizeof(switch_words[0]));
    for (backend = 1; backend >= 0; --backend) {
        struct snx_recompiled* m;
        RtAPI rt;
        CPU cpu;
        Seen seen;
        uint64_t k;
        char what[256];
        snx_recompiler_set_backend(r, backend);
        m = snx_recompile(r, SWITCH64, mem + SWITCH64, sizeof(switch_words));
        snprintf(what, sizeof(what), "switch, back end %d: snx_recompile: %s", backend, snx_recompiler_error(r));
        if (!backend) {
            /* LLVM alone, which there is no more of */
            check(!m && strstr(snx_recompiler_error(r), "LLVM alone"), what);
            if (m) snx_recompiled_free(m);
            continue;
        }
        check(m != NULL, what);
        if (!m) continue;
#if defined(__x86_64__) && !defined(_WIN32)
        /* the function, and its table's bytes after it taken for one */
        snprintf(what, sizeof(what), "the switch's functions are the x86-64 back end's (%lu of %lu)",
                 (unsigned long)snx_recompiled_x64_functions(m), (unsigned long)snx_recompiled_functions(m));
        if (backend) check(snx_recompiled_x64_functions(m) == snx_recompiled_functions(m), what);
#endif
        snx_runtime_defaults(&rt);
        rt.svc = svc64;
        rt.undef = undef64;
        rt.brk = brk64;
        switch_miss_next = rt.miss;
        rt.miss = switch_miss;
        memset(&cpu, 0, sizeof(cpu));
        memset(&seen, 0, sizeof(seen));
        cpu.mem = mem;
        cpu.rt = &rt;
        cpu.thread = &seen;
        check(snx_recompiled_bind(m, &cpu) == 0, "bind the switch");
        for (k = 0; k < 41; ++k) {
            uint64_t want = k < 32 ? (k * 5 % 32) * 7 + 3 : 999;
            cpu.x[0] = k;
            cpu.x[30] = STOP;
            snx_recompiled_run(m, &cpu, SWITCH64, STOP);
            snprintf(what, sizeof(what), "switch, back end %d: case %llu gives %llu, not %llu", backend,
                     (unsigned long long)k, (unsigned long long)cpu.x[0], (unsigned long long)want);
            check(cpu.x[0] == want && cpu.pc == STOP && !seen.brks && !seen.undefs, what);
        }
        /* each case entered at its own address: the function's dispatch block finds its leader, nothing compiled */
        switch_misses = 0;
        for (k = 0; k < 33; ++k) {
            uint64_t want = k < 32 ? k * 7 + 3 : 999;
            cpu.x[0] = 12345;
            cpu.x[30] = STOP;
            snx_recompiled_run(m, &cpu, SWITCH64 + 0x20 + 8 * k, STOP);
            snprintf(what, sizeof(what), "switch, back end %d: entered at case %llu, x0 %llu, not %llu", backend,
                     (unsigned long long)k, (unsigned long long)cpu.x[0], (unsigned long long)want);
            check(cpu.x[0] == want && cpu.pc == STOP && !seen.brks && !seen.undefs, what);
        }
        snprintf(what, sizeof(what), "switch, back end %d: the cases' entries compiled nothing (%u misses)", backend,
                 switch_misses);
        check(!switch_misses, what);
        snx_recompiled_free(m);
    }
    snx_recompiler_set_backend(r, 1);
}

/* The same past its cases, at PAST64 (llvm-mc): f: cbz w1, past; and x0, x0, #31; past: adr x9, table; adr x10, low;
   ldrb w11, [x9, x0]; add x10, x10, x11, lsl #2; br x10; low: movz x0, #777; ret; casek: movz x0, #(7k + 3); ret
   (k 0..31); high: movz x0, #888; ret. The table's 32 entries name the cases; with x1 zero the index is not masked, and
   entry 32 names low, before the first case, 33 case 0's RET, between two, and 34 high, past the last */
#define PAST64 0xB000u
static const uint32_t past_words[] = {
    0x34000041, 0x92401000, 0x10000929, 0x1000008a, 0x3860692b, 0x8b0b094a, 0xd61f0140, 0xd2806120, 0xd65f03c0,
    0xd2800060, 0xd65f03c0, 0xd2800140, 0xd65f03c0, 0xd2800220, 0xd65f03c0, 0xd2800300, 0xd65f03c0, 0xd28003e0,
    0xd65f03c0, 0xd28004c0, 0xd65f03c0, 0xd28005a0, 0xd65f03c0, 0xd2800680, 0xd65f03c0, 0xd2800760, 0xd65f03c0,
    0xd2800840, 0xd65f03c0, 0xd2800920, 0xd65f03c0, 0xd2800a00, 0xd65f03c0, 0xd2800ae0, 0xd65f03c0, 0xd2800bc0,
    0xd65f03c0, 0xd2800ca0, 0xd65f03c0, 0xd2800d80, 0xd65f03c0, 0xd2800e60, 0xd65f03c0, 0xd2800f40, 0xd65f03c0,
    0xd2801020, 0xd65f03c0, 0xd2801100, 0xd65f03c0, 0xd28011e0, 0xd65f03c0, 0xd28012c0, 0xd65f03c0, 0xd28013a0,
    0xd65f03c0, 0xd2801480, 0xd65f03c0, 0xd2801560, 0xd65f03c0, 0xd2801640, 0xd65f03c0, 0xd2801720, 0xd65f03c0,
    0xd2801800, 0xd65f03c0, 0xd28018e0, 0xd65f03c0, 0xd28019c0, 0xd65f03c0, 0xd2801aa0, 0xd65f03c0, 0xd2801b80,
    0xd65f03c0, 0xd2806f00, 0xd65f03c0, 0x08060402, 0x100e0c0a, 0x18161412, 0x201e1c1a, 0x28262422, 0x302e2c2a,
    0x38363432, 0x403e3c3a, 0x00420300,
};
static unsigned past_polls;
static void past_poll(CPU* c) {
    ++past_polls;
    atomic_store_explicit(&c->poll, 0, memory_order_relaxed);
}
static void switch_past64(struct snx_recompiler* r, uint8_t* mem) {
    int backend;
    place(mem, PAST64, past_words, sizeof(past_words) / sizeof(past_words[0]));
    for (backend = 1; backend >= 0; --backend) {
        struct snx_recompiled* m;
        RtAPI rt;
        CPU cpu = {0};
        Seen seen = {0};
        uint64_t k;
        char what[160];
        snx_recompiler_set_backend(r, backend);
        m = snx_recompile(r, PAST64, mem + PAST64, sizeof(past_words));
        if (!backend) {
            check(!m && strstr(snx_recompiler_error(r), "LLVM alone"), "past the cases: LLVM alone is refused");
            if (m) snx_recompiled_free(m);
            continue;
        }
        check(m != NULL, "past the cases: snx_recompile");
        if (!m) continue;
        snx_runtime_defaults(&rt);
        rt.svc = svc64;
        rt.undef = undef64;
        rt.brk = brk64;
        rt.poll = past_poll;
        cpu.mem = mem;
        cpu.rt = &rt;
        cpu.thread = &seen;
        check(snx_recompiled_bind(m, &cpu) == 0, "past the cases: bind");
        for (k = 0; k < 70; ++k) { /* each index, and each again with a poll pending */
            uint64_t index = k % 35, want = index < 32 ? 7 * index + 3 : index == 32 ? 777 : index == 33 ? 33 : 888;
            cpu.x[0] = index;
            cpu.x[1] = 0;
            cpu.x[30] = STOP;
            past_polls = 0;
            atomic_store_explicit(&cpu.poll, k >= 35, memory_order_relaxed);
            snx_recompiled_run(m, &cpu, PAST64, STOP);
            snprintf(what, sizeof(what), "past the cases, back end %d: index %llu gives %llu, not %llu", backend,
                     (unsigned long long)index, (unsigned long long)cpu.x[0], (unsigned long long)want);
            check(cpu.x[0] == want && cpu.pc == STOP && !seen.brks && !seen.undefs, what);
            snprintf(what, sizeof(what), "past the cases, back end %d: index %llu, %u polls taken, not %d", backend,
                     (unsigned long long)index, past_polls, k >= 35);
            check(past_polls == (k >= 35), what);
        }
        snx_recompiled_free(m);
    }
    snx_recompiler_set_backend(r, 1);
}

static void run64(struct snx_recompiler* r, const char* backend, uint8_t* mem, const char* keep_ir) {
    struct snx_recompiled *m, *first;
    RtAPI rt;
    CPU cpu;
    Seen seen;
    uint64_t hint = FAR, lo, hi;
    char what[256];
    snx_runtime_defaults(&rt);
    rt.svc = svc64;
    rt.undef = undef64;
    rt.brk = brk64;
    check(rt.ticks && rt.ticks() != 0 && rt.poll && rt.sev && rt.wfe && rt.fpcr_changed && rt.note_hint,
          "snx_runtime_defaults fills the other hooks");
    /* the IR was LLVM's: a folder for it is refused, naming it, and the code is made all the same */
    if (keep_ir) {
        snprintf(what, sizeof(what), "set_keep_ir is refused: %s", snx_recompiler_error(r));
        check(snx_recompiler_set_keep_ir(r, keep_ir) == -1 && strstr(snx_recompiler_error(r), keep_ir) &&
                  strstr(snx_recompiler_error(r), "no IR is kept"),
              what);
    }
    /* the program as a buffer at its guest address */
    m = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
    snprintf(what, sizeof(what), "%s: snx_recompile: %s", backend, snx_recompiler_error(r));
    check(m != NULL, what);
    if (!m) return;
    snx_recompiled_range(m, &lo, &hi);
    check(lo == CODE64 && hi == CODE64 + A64_SIZE, "the module's range is the buffer's");
    snprintf(what, sizeof(what), "%s: two functions found (entry and helper), %lu reported", backend,
             (unsigned long)snx_recompiled_functions(m));
    check(snx_recompiled_functions(m) == 2, what);
    check(snx_recompiled_fallbacks(m) == 0, "every instruction has an emitter");
    snprintf(what, sizeof(what), "%s: every function the native back end's (%lu of %lu)", backend,
             (unsigned long)snx_recompiled_x64_functions(m), (unsigned long)snx_recompiled_functions(m));
    check(snx_recompiled_x64_functions(m) == snx_recompiled_functions(m), what);
    /* a CPU without the hooks is refused */
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = mem;
    check(snx_recompiled_bind(m, &cpu) == -1, "bind refuses a CPU without a runtime table");
    cpu.rt = &rt;
    check(snx_recompiled_bind(m, &cpu) == 0, "bind");
    check(cpu.disp && cpu.code_lo == CODE64 && cpu.code_hi == CODE64 + A64_SIZE, "bind set the dispatch and range");
    /* the recompiled path: entry, loop, the call, the SVC, the store, the return to STOP */
    memset(&seen, 0, sizeof(seen));
    cpu.thread = &seen;
    cpu.x[2] = STORE;
    cpu.x[30] = STOP;
    snx_recompiled_run(m, &cpu, CODE64, STOP);
    snprintf(what, sizeof(what), "%s: one SVC #0x42 with x0 = 1055 (saw %u, imm %#x, x0 %llu)", backend, seen.svcs,
             seen.svc_imm, (unsigned long long)seen.svc_x0);
    check(seen.svcs == 1 && seen.svc_imm == 0x42 && seen.svc_x0 == 1055, what);
    check(cpu.x[0] == 7 && read64(mem + STORE) == 7, "the SVC's result reached x0 and guest memory");
    check(cpu.pc == STOP && !seen.brks && !seen.undefs, "the entry returned to STOP");
    /* the JIT path: far was never found, so the miss compiles it, and its BRK comes from that code */
    memset(&seen, 0, sizeof(seen));
    cpu.x[0] = 0;
    cpu.x[30] = STOP;
    snx_recompiled_run(m, &cpu, FAR, STOP);
    snprintf(what, sizeof(what), "%s: BRK #7 at far, compiled at its miss (brks %u imm %u pc %#llx x0 %llu)", backend,
             seen.brks, seen.brk_imm, (unsigned long long)seen.brk_pc, (unsigned long long)cpu.x[0]);
    check(seen.brks == 1 && seen.brk_imm == 7 && seen.brk_pc == FAR + 4 && cpu.x[0] == 99, what);
    check(snx_recompiled_lookup(m, FAR) && snx_recompiled_lookup(m, FAR) != snx_recompiled_lookup(m, CODE64),
          "far has code of its own, the JIT's");
    check(!snx_recompiled_lookup(m, CODE64 + A64_SIZE + 0x100000), "nothing compiles outside the code ranges");
    /* IC IVAU, compiled at its miss: the runtime hears of the line, 64 bytes from x0 rounded down. Without the hook
       (snx_runtime_defaults leaves it NULL) it does nothing */
    memset(&seen, 0, sizeof(seen));
    cpu.x[0] = 0x123457F;
    cpu.x[30] = STOP;
    snx_recompiled_run(m, &cpu, IVAU, STOP);
    check(cpu.pc == STOP && !seen.changes, "IC IVAU without RtAPI.code_changed does nothing");
    rt.code_changed = code_changed64;
    cpu.x[30] = STOP;
    snx_recompiled_run(m, &cpu, IVAU, STOP);
    snprintf(what, sizeof(what), "%s: IC IVAU reaches RtAPI.code_changed with its line (%u calls, %#llx)", backend,
             seen.changes, (unsigned long long)seen.changed);
    check(seen.changes == 1 && seen.changed == 0x1234540 && cpu.pc == STOP, what);
    rt.code_changed = NULL;
    /* an undefined word */
    memset(&seen, 0, sizeof(seen));
    snx_recompiled_run(m, &cpu, UNDEF, STOP);
    check(seen.undefs == 1 && seen.undef_pc == UNDEF && seen.undef_word == 0, "UDF #0 reaches the undef hook");
    /* a second module while the first is still loaded (two modules must never alias, whatever the OS names a memory
       file), told about far: it is recompiled too, and its BRK comes from generated code */
    check(!snx_recompiler_set_keep_ir(r, NULL), "keep_ir off");
    first = m;
    m = snx_recompile_ranges(r, &(struct snx_code_range){CODE64, mem + CODE64, A64_SIZE}, 1, &hint, 1);
    snprintf(what, sizeof(what), "%s: snx_recompile_ranges with a hint: %s", backend, snx_recompiler_error(r));
    check(m != NULL, what);
    if (!m) return;
    check(snx_recompiled_functions(m) == 3, "the hint made far a function");
    check(snx_recompiled_lookup(m, FAR) != snx_recompiled_lookup(m, UNDEF), "far is recompiled now");
    check(snx_recompiled_lookup(m, CODE64) != snx_recompiled_lookup(first, CODE64),
          "the second module has code of its own, not the first module's");
    check(snx_recompiled_lookup(first, FAR) != snx_recompiled_lookup(m, FAR),
          "the first module's far is its JIT's, the second's recompiled");
    snx_recompiled_free(first);
    check(snx_recompiled_bind(m, &cpu) == 0, "bind the second module");
    memset(&seen, 0, sizeof(seen));
    cpu.x[0] = 0;
    cpu.x[30] = STOP;
    snx_recompiled_call(m, &cpu, FAR);
    check(seen.brks == 1 && seen.brk_imm == 7 && seen.brk_pc == FAR + 4 && cpu.x[0] == 99 && cpu.pc == STOP,
          "BRK #7 from recompiled far");
    memset(&seen, 0, sizeof(seen));
    cpu.x[0] = 0;
    cpu.x[2] = STORE;
    cpu.x[30] = STOP;
    mem[STORE] = 0;
    snx_recompiled_run(m, &cpu, CODE64, STOP);
    check(seen.svcs == 1 && cpu.x[0] == 7 && read64(mem + STORE) == 7, "the second module runs entry too");
    snx_recompiled_free(m);
}

typedef struct {
    size_t count;
    uint64_t start, end;
} Invalidated;

static void invalidated(void* context, uint64_t start, uint64_t end) {
    Invalidated* seen = (Invalidated*)context;
    ++seen->count;
    seen->start = start;
    seen->end = end;
}

static void invalidation_fallback(CPU* cpu, uint64_t pc) {
    unsigned* calls = (unsigned*)cpu->thread;
    ++*calls;
    cpu->pc = STOP;
    (void)pc;
}


#ifndef _WIN32
/* A spin on memory another thread writes (spin: mov x8,#SPIN_FLAG; ldr w1,[x8]; cbz w1,spin; ret), with nothing to
   poll for: the loop must load the word on every pass, so it ends soon after the other thread's store. Its only call
   is rt->poll on the poll's cold path, and a guest load isn't volatile, so without the poll's compiler barrier LLVM's
   GVN moves the load of an address the loop builds from constants onto that path (an address in a register the loop
   doesn't write is reloaded after the call, which keeps its load), and the loop spins on CPU.poll alone. The writer
   flags the CPU after 2 s, so that such a loop ends too, and the check fails */
#define SPIN      0x3000u
#define SPIN_FLAG 0x9000u
static const uint32_t spin_words[] = {0xd2920008, 0xb9400101, 0x34ffffc1, 0xd65f03c0};
typedef struct {
    uint8_t* mem;
    CPU* cpu;
    _Atomic int done;
    int flagged;
} Spin;
static void nap(long ms) {
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t)) {
    }
}
static void* spin_writer(void* arg) {
    Spin* s = (Spin*)arg;
    long ms;
    nap(50);
    atomic_store_explicit((_Atomic uint32_t*)(s->mem + SPIN_FLAG), 1u, memory_order_release);
    for (ms = 0; ms < 2000 && !atomic_load_explicit(&s->done, memory_order_acquire); ms += 10) nap(10);
    if (!atomic_load_explicit(&s->done, memory_order_acquire)) {
        s->flagged = 1;
        atomic_store_explicit(&s->cpu->poll, 1u, memory_order_release);
    }
    return NULL;
}
static void spin64(struct snx_recompiler* r, uint8_t* mem) {
    struct snx_recompiled* m;
    Seen seen = {0};
    RtAPI rt;
    CPU cpu = {0};
    Spin s;
    pthread_t writer;
    place(mem, SPIN, spin_words, sizeof(spin_words) / sizeof(spin_words[0]));
    m = snx_recompile(r, SPIN, mem + SPIN, sizeof(spin_words));
    check(m != NULL, "spin: a module");
    if (!m) return;
    snx_runtime_defaults(&rt);
    rt.svc = svc64;
    rt.undef = undef64;
    rt.brk = brk64;
    cpu.mem = mem;
    cpu.rt = &rt;
    cpu.thread = &seen;
    check(snx_recompiled_bind(m, &cpu) == 0, "spin: bind");
    cpu.x[30] = STOP;
    s.mem = mem;
    s.cpu = &cpu;
    s.done = s.flagged = 0;
    check(pthread_create(&writer, NULL, spin_writer, &s) == 0, "spin: the writer thread");
    snx_recompiled_run(m, &cpu, SPIN, STOP);
    atomic_store_explicit(&s.done, 1, memory_order_release);
    pthread_join(writer, NULL);
    check(!s.flagged && cpu.x[1] == 1, "spin: a loop sees another thread's store without being polled");
    snx_recompiled_free(m);
}
#endif

static void invalidate64(struct snx_recompiler* r, uint8_t* mem) {
    const uint64_t helper = CODE64 + 11 * 4;
    struct snx_recompiled* module;
    Invalidated seen = {0};
    RtAPI rt;
    CPU cpu = {0};
    unsigned fallback_calls = 0;
    snx_recompiler_set_hook_direct_calls(r, 1);
    module = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
    check(module != NULL, "invalidation: compile a hook-enabled module");
    if (!module) {
        snx_recompiler_set_hook_direct_calls(r, 0);
        return;
    }
    check(snx_recompiled_invalidate(module, helper, 0, invalidated, &seen) == 0,
          "invalidation: zero-size range is ignored");
    check(snx_recompiled_invalidate(module, UINT64_MAX - 3, 8, invalidated, &seen) == 0,
          "invalidation: overflowing range is ignored");
    check(snx_recompiled_function(module, helper) != NULL, "invalidation: helper starts compiled");
    check(snx_recompiled_invalidate(module, helper, 4, invalidated, &seen) == 1,
          "invalidation: retire the overlapping helper before the JIT starts");
    check(seen.count == 1 && seen.start <= helper && seen.end > helper,
          "invalidation: callback reports the helper's source span");
    check(snx_recompiled_function(module, helper) == NULL &&
              snx_recompiled_function(module, CODE64) != NULL,
          "invalidation: clear only the helper's dispatch entries");
    {
        /* the module's own copy of the code, which its JIT compiles from, follows the guest's bytes */
        uint32_t words[2] = {0xd503201f, 0xd65f03c0}; /* nop; ret */
        uint8_t guest[sizeof(words)];                 /* the words as the guest has them in memory */
        place(guest, 0, words, 2);
        check(snx_recompiled_update_code(module, helper, guest, sizeof(guest)) == sizeof(guest),
              "update_code: the words inside the module's ranges are taken");
        check(snx_recompiled_update_code(module, helper + 2, guest, 4) == 0,
              "update_code: a misaligned address is refused");
        check(snx_recompiled_update_code(module, 0x10, guest, 4) == 0,
              "update_code: an address outside the ranges is not taken");
        check(snx_recompiled_update_code(module, helper, mem + helper, sizeof(words)) == sizeof(words),
              "update_code: the original bytes go back");
    }
    snx_runtime_defaults(&rt);
    rt.svc = svc64;
    rt.undef = undef64;
    rt.brk = brk64;
    cpu.mem = mem;
    cpu.rt = &rt;
    cpu.thread = &fallback_calls;
    snx_recompiled_set_fallback(module, invalidation_fallback);
    snx_recompiled_call(module, &cpu, helper);
    check(fallback_calls == 1 && cpu.pc == STOP,
          "invalidation: the cleared entry runs the embedder's live-code fallback");
    snx_recompiled_free(module);
    snx_recompiler_set_hook_direct_calls(r, 0);
}

/* ---- A32 ---- */
typedef struct {
    unsigned calls;
    uint32_t imm;
    uint64_t number, r0;
} Seen32;
static int svc32(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]) {
    Seen32* s = (Seen32*)context;
    ++s->calls;
    s->imm = imm;
    s->number = number;
    s->r0 = arguments[0];
    return 1; /* ends the run */
}

static void run32(struct snx_recompiler* r, uint8_t* mem) {
    struct snx_recompiled32* m;
    SnxArmCPU cpu;
    Seen32 seen;
    char what[256];
    int exit_code;
    m = snx_recompile32(r, CODE32, sizeof(a32_words));
    snprintf(what, sizeof(what), "snx_recompile32: %s", snx_recompiler_error(r));
    check(m != NULL, what);
    if (!m) return;
    memset(&cpu, 0, sizeof(cpu));
    memset(&seen, 0, sizeof(seen));
    cpu.mem = mem;
    cpu.mem_size = MEMORY;
    cpu.cpsr = 0x10;
    cpu.r[15] = CODE32;
    exit_code = snx_recompiled32_run(m, &cpu, svc32, &seen);
    snprintf(what, sizeof(what),
             "A32: svc #0 with r7 = 0x11, r0 = 7 ended the run (exit %d, calls %u, number %#llx, "
             "r0 %llu)",
             exit_code, seen.calls, (unsigned long long)seen.number, (unsigned long long)seen.r0);
    check(exit_code == 0 && seen.calls == 1 && seen.imm == 0 && seen.number == 0x11 && seen.r0 == 7, what);
    check(cpu.r[15] == CODE32 + 4 * 4, "the A32 PC is past the SVC");
    snx_recompiled32_free(m);
}

/* snx_recompiler_set_progress: the totals first with nothing done, then each unit's work as it finishes, ending at the
   totals; nothing once the callback is cleared */
typedef struct {
    int calls, ordered;
    unsigned long long first_done, first_total, done, total, functions_done, functions_total;
} Progress;
static void progress(void* user, unsigned long long weight_done, unsigned long long weight_total,
                     unsigned long long functions_done, unsigned long long functions_total) {
    Progress* p = (Progress*)user;
    if (!p->calls++) {
        p->first_done = weight_done | functions_done;
        p->first_total = weight_total;
    } else if (weight_done < p->done || weight_total != p->total || functions_total != p->functions_total)
        p->ordered = 0;
    p->done = weight_done;
    p->total = weight_total;
    p->functions_done = functions_done;
    p->functions_total = functions_total;
}
static void progress64(struct snx_recompiler* r, uint8_t* mem) {
    Progress p = {0, 1, 0, 0, 0, 0, 0, 0};
    snx_recompiler_set_progress(r, progress, &p);
    struct snx_recompiled* m = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
    check(m != NULL, "progress: a module");
    check(p.calls >= 2 && p.first_done == 0 && p.first_total > 0, "progress: the totals first, nothing done");
    check(p.ordered && p.done == p.total && p.total == p.first_total, "progress: the work grows to the totals");
    check(m && p.functions_done == p.functions_total && p.functions_total == snx_recompiled_functions(m),
          "progress: every function counted");
    snx_recompiled_free(m);
    int calls = p.calls;
    snx_recompiler_set_progress(r, NULL, NULL);
    m = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
    check(m != NULL && p.calls == calls, "progress: nothing reported once cleared");
    snx_recompiled_free(m);
}

/* The code units, by instructions plus four per leader. entry calls wide, 4501 instructions in one block, then branchy,
   2001 instructions in about 2000 blocks (cbz x1,.+8; add x0,x0,#1, a thousand times). With 4 jobs the units are
   {entry, wide} and {branchy}, which weighs about 10000 to wide's 4500 though it has fewer instructions: the progress
   callback sees each unit's functions and weight as it finishes (which of the two is taken first, the heavier,
   recompiler_library_schedule checks) */
#define SCHEDULE64   0x100000u
#define WIDE_WORDS   4501u
#define BRANCH_PAIRS 1000u
/* each unit as it finishes: its functions and its weight, from the progress callback's running totals */
typedef struct {
    unsigned calls;
    unsigned long long weight, functions, unit_weight[4], unit_functions[4];
} Schedule;
static void schedule_progress(void* user, unsigned long long weight_done, unsigned long long weight_total,
                              unsigned long long functions_done, unsigned long long functions_total) {
    Schedule* s = (Schedule*)user;
    (void)weight_total, (void)functions_total;
    if (!functions_done) return; /* the totals, first */
    if (s->calls < 4) {
        s->unit_weight[s->calls] = weight_done - s->weight;
        s->unit_functions[s->calls] = functions_done - s->functions;
    }
    ++s->calls;
    s->weight = weight_done;
    s->functions = functions_done;
}
static void schedule64(struct snx_recompiler* r) {
    const uint64_t wide = SCHEDULE64 + 3 * 4, branchy = wide + WIDE_WORDS * 4;
    const size_t words = 3 + WIDE_WORDS + 2 * BRANCH_PAIRS + 1;
    uint32_t* code = (uint32_t*)calloc(words, sizeof *code);
    uint8_t* bytes = (uint8_t*)calloc(words, 4);
    check(code && bytes, "schedule: memory");
    if (!code || !bytes) {
        free(code);
        free(bytes);
        return;
    }
    size_t n = 0;
    code[n++] = 0x94000000u | (uint32_t)((wide - SCHEDULE64) / 4);          /* bl wide */
    code[n++] = 0x94000000u | (uint32_t)((branchy - (SCHEDULE64 + 4)) / 4); /* bl branchy */
    code[n++] = 0xd65f03c0u;                                                /* ret */
    for (uint32_t i = 0; i + 1 < WIDE_WORDS; ++i) code[n++] = 0x91000400u;  /* add x0,x0,#1 */
    code[n++] = 0xd65f03c0u;
    for (uint32_t i = 0; i < BRANCH_PAIRS; ++i) {
        code[n++] = 0xb4000041u; /* cbz x1,.+8 */
        code[n++] = 0x91000400u;
    }
    code[n++] = 0xd65f03c0u;
    for (size_t i = 0; i < n; ++i) {
        bytes[4 * i] = (uint8_t)code[i];
        bytes[4 * i + 1] = (uint8_t)(code[i] >> 8);
        bytes[4 * i + 2] = (uint8_t)(code[i] >> 16);
        bytes[4 * i + 3] = (uint8_t)(code[i] >> 24);
    }
    Schedule s = {0};
    snx_recompiler_set_jobs(r, 4);
    snx_recompiler_set_progress(r, schedule_progress, &s);
    struct snx_recompiled* m = snx_recompile(r, SCHEDULE64, bytes, n * 4);
    snx_recompiler_set_progress(r, NULL, NULL);
    check(m != NULL && snx_recompiled_functions(m) == 3, "schedule: entry, wide and branchy found");
    const unsigned alone = s.unit_functions[0] == 1 ? 0 : 1; /* branchy's unit, whichever finished first */
    char what[192];
    snprintf(what, sizeof what,
             "schedule: 2 units, branchy alone and the heavier (%u: %llu and %llu functions, weights"
             " %llu and %llu)",
             s.calls, s.unit_functions[0], s.unit_functions[1], s.unit_weight[0], s.unit_weight[1]);
    check(s.calls == 2 && s.unit_functions[alone] == 1 && s.unit_functions[!alone] == 2 &&
              s.unit_weight[alone] > s.unit_weight[!alone],
          what);
    snx_recompiled_free(m);
    snx_recompiler_set_jobs(r, 2);
    free(bytes);
    free(code);
}

/* snx_recompiler_set_live_code: the JIT compiles far as its bytes are when it is first reached, after the recompile
   (here mov x0,#42 instead of #99), not as they were when the module was made; without it, the module's copy */
static void live64(struct snx_recompiler* r, uint8_t* mem) {
    const uint32_t mov99 = 0xd2800c60, mov42 = 0xd2800540;
    for (int live = 0; live < 2; ++live) {
        struct snx_recompiled* m;
        RtAPI rt;
        CPU cpu = {0};
        Seen seen = {0};
        snx_recompiler_set_live_code(r, live);
        m = snx_recompile(r, CODE64, mem + CODE64, A64_SIZE);
        check(m != NULL, "live code: a module");
        if (!m) continue;
        memcpy(mem + FAR, &mov42, 4);
        snx_runtime_defaults(&rt);
        rt.svc = svc64;
        rt.undef = undef64;
        rt.brk = brk64;
        cpu.mem = mem;
        cpu.rt = &rt;
        cpu.thread = &seen;
        check(snx_recompiled_bind(m, &cpu) == 0, "live code: bind");
        cpu.x[30] = STOP;
        snx_recompiled_run(m, &cpu, FAR, STOP);
        check(seen.brks == 1 && cpu.x[0] == (live ? 42u : 99u),
              live ? "live code: the JIT compiles far as it is now" : "a module's JIT compiles far from its copy");
        memcpy(mem + FAR, &mov99, 4);
        snx_recompiled_free(m);
    }
    snx_recompiler_set_live_code(r, 0);
}

/* A function patched again after the JIT built code for its first patch: the AOT retirement and the JIT's own drop of
   the unit both clear the pc's slot, and whichever comes second must still leave the pc compilable. The retirement used
   to empty the slot unconditionally, so the unit's drop (a compare-and-swap against its own code) failed and kept the
   pc's pending bit: every later miss there answered NULL without queueing, an undefined instruction for good */
static unsigned repatch_undefs;
static void repatch_undef(CPU* c, uint64_t pc, uint32_t word) {
    (void)word;
    ++repatch_undefs;
    c->pc = 0;
    (void)pc;
}
static void repatch_svc(CPU* c, uint32_t imm) {
    (void)c;
    (void)imm;
}
static void repatch_brk(CPU* c, uint64_t pc, uint32_t imm) {
    (void)pc;
    (void)imm;
    c->pc = 0;
}
static uint64_t repatch_run(struct snx_recompiled* module, uint8_t* mem, RtAPI* rt, uint64_t entry) {
    CPU cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = mem;
    cpu.rt = rt;
    cpu.x[31] = 0x6000;
    if (snx_recompiled_bind(module, &cpu) != 0) return UINT64_MAX;
    snx_recompiled_run(module, &cpu, entry, 0);
    return cpu.x[0];
}

static void test_update_code_live(void) {
    static uint8_t mem[MEMORY];
    const uint32_t program[2] = {0x91000400, 0xd65f03c0}; /* add x0,x0,#1; ret */
    const uint32_t other = 0xd503201f;                      /* nop */
    uint8_t guest[4];
    struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_NATIVE);
    check(r != NULL, "update_code live: a recompiler");
    if (!r) return;
    place(mem, CODE64, program, 2);
    snx_recompiler_set_live_code(r, 1);
    struct snx_recompiled* module = snx_recompile(r, CODE64, mem + CODE64, sizeof(program));
    check(module != NULL, "update_code live: the module compiles");
    if (!module) {
        snx_recompiler_free(r);
        return;
    }
    place(guest, 0, &other, 1);
    check(snx_recompiled_update_code(module, CODE64, guest, sizeof(guest)) == 0,
          "update_code live: a live-code module has no copy to update");
    check(memcmp(mem + CODE64, guest, sizeof(guest)) != 0, "update_code live: the caller's bytes are not written");
    snx_recompiled_free(module);
    snx_recompiler_free(r);
}

static void test_repatch(void) {
    enum { AT = 0x4000, PATCHED = AT + 0x40 }; /* the entry's line and the patched word's line differ */
    static uint8_t mem[MEMORY];
    struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_NATIVE);
    check(r != NULL, "repatch: a recompiler");
    if (!r) return;
    uint32_t words[18];
    for (unsigned i = 0; i < 16; ++i) words[i] = 0xd503201f; /* nop */
    words[16] = 0x91000400;                                   /* add x0,x0,#1 */
    words[17] = 0xd65f03c0;                                   /* ret */
    place(mem, AT, words, 18);
    snx_recompiler_set_hook_direct_calls(r, 1);
    struct snx_recompiled* module = snx_recompile(r, AT, mem + AT, sizeof(words));
    check(module != NULL, "repatch: the module compiles");
    if (!module) {
        snx_recompiler_free(r);
        return;
    }
    RtAPI rt;
    snx_runtime_defaults(&rt);
    rt.svc = repatch_svc;
    rt.undef = repatch_undef;
    rt.brk = repatch_brk;
    check(repatch_run(module, mem, &rt, AT) == 1, "repatch: the AOT code runs first");
    int all = 1;
    for (unsigned k = 2; k <= 6; ++k) {
        uint32_t word = 0x91000000u | (k << 10); /* add x0,x0,#k */
        place(mem, PATCHED, &word, 1);
        snx_recompiled_update_code(module, PATCHED, mem + PATCHED, sizeof(word));
        snx_recompiled_invalidate(module, PATCHED, 64, NULL, NULL);
        if (repatch_run(module, mem, &rt, AT) != k) all = 0;
    }
    check(all && repatch_undefs == 0,
          "repatch: each patch of the same function runs, none is an undefined instruction");
    snx_recompiled_free(module);
    snx_recompiler_free(r);
}

#ifndef _WIN32
/* ---- snx_recompiled_update_code while the module's JIT compiles ----
   UPDATE: ret, the module's entry and its AOT function. UPDATE_FN: mov x0,#0; add x0,x0,#1; FLIP; ret, which only
   the JIT compiles, FLIP a nop or cbnz xzr,.-4 (never taken: x0 is 1 either way). UPDATE_FAR: nop; ret, never run */
#define UPDATE      0x5000u
#define UPDATE_FN   (UPDATE + 0x10)
#define UPDATE_FLIP (UPDATE_FN + 8)
#define UPDATE_FAR  (UPDATE + 0x400)
#define UPDATE_END  (UPDATE_FAR + 8)
#define NOP_WORD    0xd503201fu
#define RET_WORD    0xd65f03c0u
typedef struct {
    struct snx_recompiled* m;
    uint64_t at;       /* the word it writes */
    uint32_t words[2]; /* in turn */
    long pause_us;     /* about this long between two writes, 0 for none */
    _Atomic int stop;
} UpdateWriter;
static void nap_us(long us) {
    struct timespec t;
    t.tv_sec = us / 1000000;
    t.tv_nsec = (us % 1000000) * 1000L;
    while (nanosleep(&t, &t)) {}
}
static void* update_writer(void* arg) {
    UpdateWriter* w = (UpdateWriter*)arg;
    unsigned seed = 1;
    for (unsigned n = 0; !atomic_load_explicit(&w->stop, memory_order_acquire); ++n) {
        uint8_t bytes[4];
        place(bytes, 0, &w->words[n & 1], 1);
        snx_recompiled_update_code(w->m, w->at, bytes, sizeof bytes);
        seed = seed * 1103515245u + 12345u;
        if (w->pause_us) nap_us(w->pause_us / 2 + (long)((seed >> 16) % (unsigned)w->pause_us));
    }
    return NULL;
}
/* x0 of a run of UPDATE_FN, UINT64_MAX when it reached no code */
static uint64_t update_call(struct snx_recompiled* m, uint8_t* mem, RtAPI* rt) {
    CPU cpu = {0};
    Seen seen = {0};
    cpu.mem = mem;
    cpu.rt = rt;
    cpu.thread = &seen;
    cpu.x[30] = STOP;
    if (snx_recompiled_bind(m, &cpu)) return UINT64_MAX;
    snx_recompiled_run(m, &cpu, UPDATE_FN, STOP);
    return seen.undefs ? UINT64_MAX : cpu.x[0];
}
static struct snx_recompiled* update_module(struct snx_recompiler* r, uint8_t* mem, RtAPI* rt) {
    const uint32_t ret = RET_WORD, fn[] = {0xd2800000, 0x91000400, NOP_WORD, RET_WORD}, far[] = {NOP_WORD, RET_WORD};
    memset(mem + UPDATE, 0, UPDATE_END - UPDATE);
    place(mem, UPDATE, &ret, 1);
    place(mem, UPDATE_FN, fn, 4);
    place(mem, UPDATE_FAR, far, 2);
    snx_runtime_defaults(rt);
    rt->svc = svc64;
    rt->undef = undef64;
    rt->brk = brk64;
    return snx_recompile(r, UPDATE, mem + UPDATE, UPDATE_END - UPDATE);
}

static double update_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* A word that changes while the JIT compiles its function: discovery and the emitter see one version of it. Read
   again by the emitter, a cbnz where discovery had read the nop branched to UPDATE_FN + 4, a block discovery never
   made, and the JIT's thread died in LLVMBuildBr within the first runs. A batch whose word changed is compiled again,
   so the runs stop after 30 s on a host slow enough to lose most batches that way */
static void update_flip64(struct snx_recompiler* r, uint8_t* mem) {
    RtAPI rt;
    struct snx_recompiled* m = update_module(r, mem, &rt);
    pthread_t writer;
    unsigned wrong = 0, runs = 0;
    check(m != NULL, "update flip: a module");
    if (!m) return;
    UpdateWriter w = {.m = m, .at = UPDATE_FLIP, .words = {NOP_WORD, 0xb5ffffffu /* cbnz xzr,.-4 */}, .pause_us = 5000};
    if (pthread_create(&writer, NULL, update_writer, &w)) {
        check(0, "update flip: the writer thread");
        snx_recompiled_free(m);
        return;
    }
    for (double end = update_seconds() + 30; runs < 100 && update_seconds() < end; ++runs) {
        snx_recompiled_invalidate(m, UPDATE_FN, 16, NULL, NULL);
        wrong += update_call(m, mem, &rt) != 1;
    }
    atomic_store_explicit(&w.stop, 1, memory_order_release);
    pthread_join(writer, NULL);
    check(runs && !wrong, "update flip: each run of a function while another thread flips one of its words gives 1");
    snx_recompiled_free(m);
}

typedef struct {
    struct snx_recompiled* m;
    uint8_t* mem;
    RtAPI* rt;
    uint64_t x0;
    _Atomic int done;
} UpdateRun;
static void* update_runner(void* arg) {
    UpdateRun* u = (UpdateRun*)arg;
    u->x0 = update_call(u->m, u->mem, u->rt);
    atomic_store_explicit(&u->done, 1, memory_order_release);
    return NULL;
}
/* 1 when UPDATE_FN, invalidated, runs to x0 = 1 within 20 s while two threads write as w says without a pause: two,
   so that one of them is writing whenever the other is off its CPU */
static int update_runs_beside(struct snx_recompiled* m, uint8_t* mem, RtAPI* rt, UpdateWriter* w) {
    UpdateRun u = {.m = m, .mem = mem, .rt = rt};
    pthread_t writers[2], runner;
    int started = 0;
    snx_recompiled_invalidate(m, UPDATE_FN, 16, NULL, NULL);
    atomic_store_explicit(&w->stop, 0, memory_order_relaxed);
    while (started < 2 && !pthread_create(&writers[started], NULL, update_writer, w)) ++started;
    int ran = started == 2 && !pthread_create(&runner, NULL, update_runner, &u);
    for (long ms = 0; ran && ms < 20000 && !atomic_load_explicit(&u.done, memory_order_acquire); ms += 10) nap(10);
    int in_time = ran && atomic_load_explicit(&u.done, memory_order_acquire);
    atomic_store_explicit(&w->stop, 1, memory_order_release); /* a run still waiting gets its code once they stop */
    while (started) pthread_join(writers[--started], NULL);
    if (ran) pthread_join(runner, NULL);
    return in_time && u.x0 == 1;
}

/* Updates that leave the words a JIT batch read as they were, of another range of the module or the same bytes again,
   keep the batch: thrown away for any update of the module, no batch finished while a writer never paused, and a run
   that took milliseconds waited until the writer stopped */
static void update_elsewhere64(struct snx_recompiler* r, uint8_t* mem) {
    RtAPI rt;
    struct snx_recompiled* m = update_module(r, mem, &rt);
    check(m != NULL, "update elsewhere: a module");
    if (!m) return;
    UpdateWriter far = {.m = m, .at = UPDATE_FAR, .words = {NOP_WORD, 0xd2800021u /* mov x1,#1 */}};
    check(update_runs_beside(m, mem, &rt, &far),
          "update elsewhere: a function compiles while another word of its module changes without a pause");
    UpdateWriter same = {.m = m, .at = UPDATE_FLIP, .words = {NOP_WORD, NOP_WORD}};
    check(update_runs_beside(m, mem, &rt, &same),
          "update elsewhere: a function compiles while its own word is written again with the bytes it holds");
    snx_recompiled_free(m);
}
#endif

/* the lines the library says while the old flag is tried */
static unsigned old_flag_lines;
static char old_flag_line_text[512];
static void old_flag_line(const char* line) {
    ++old_flag_lines;
    snprintf(old_flag_line_text, sizeof old_flag_line_text, "%s", line);
}

int main(int argc, char** argv) {
    uint8_t* mem = (uint8_t*)calloc(MEMORY, 1);
    struct snx_recompiler* r;
    char *keep_ir, *cache;
    int verbose = argc > 1 && !strcmp(argv[1], "--verbose");
    if (!mem) return 2;
    place(mem, CODE64, a64_words, sizeof(a64_words) / sizeof(a64_words[0]));
    place(mem, CODE32, a32_words, sizeof(a32_words) / sizeof(a32_words[0]));
    test_update_code_live();
    test_repatch();
    check(snx_recompiler_abi() == SNX_ABI, "the library's ABI is this cpu.h's");
    check(!snx_recompiler_has(0) && !snx_recompiler_new(0), "flags must name a backend");
    check(!snx_recompiler_has(4) && !snx_recompiler_new(4), "flags must name the native back end");
    {
        /* earlier versions' name of the flag, LLVM's: the native back end's recompiler, said once */
        old_flag_lines = 0;
        snx_error_sink(old_flag_line);
        struct snx_recompiler* a = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
        struct snx_recompiler* b = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
        snx_error_sink(NULL);
        check(snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM) && a && b, "the old flag makes a recompiler");
        check(old_flag_lines == 1 && strstr(old_flag_line_text, "SNX_RECOMPILER_FLAG_LLVM (1)") &&
                  strstr(old_flag_line_text, "SNX_RECOMPILER_FLAG_NATIVE (2)"),
              "the old flag said once, with both names and values");
        snx_recompiler_free(a);
        snx_recompiler_free(b);
    }
    check(snx_recompiler_has(SNX_RECOMPILER_FLAG_NATIVE), "this host's native back end makes the code");
    keep_ir = snx_temp_dir(NULL);
    check(keep_ir != NULL, "a folder for the IR");
    cache = snx_temp_dir(NULL);
    check(cache != NULL, "a folder for the caches");
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_NATIVE);
    check(r != NULL, "snx_recompiler_new(LLVM)");
    if (r) {
        snx_recompiler_set_verbose(r, verbose);
        check(snx_recompiler_set_optimization(r, 'x') == -1 && snx_recompiler_error(r)[0], "a bad -O level is refused");
        check(snx_recompiler_set_optimization(r, '2') == 0, "-O2");
        check(snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_ANDROID) == 0, "the android A64 target");
        check(snx_recompiler_set_a64_target(r, 3) == -1 && snx_recompiler_error(r)[0],
              "an unknown A64 target is refused");
        check(snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_MAX) == 0 &&
                  snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_SWITCH) == 0,
              "the Switch and max A64 targets");
        snx_recompiler_set_jobs(r, 2);
        run64(r, "native", mem, keep_ir);
        switch64(r, mem);
        switch_past64(r, mem);
        progress64(r, mem);
        schedule64(r);
        invalidate64(r, mem);
        live64(r, mem);
#ifndef _WIN32
        spin64(r, mem);
        update_flip64(r, mem);
        update_elsewhere64(r, mem);
#endif
        jit_cache(r, mem, cache);
        run32(r, mem);
        snx_recompiler_free(r);
    }
    if (keep_ir) {
        snx_remove_tree(keep_ir);
        free(keep_ir);
    }
    if (cache) {
        snx_remove_tree(cache);
        free(cache);
    }
    free(mem);
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("recompiler library: ok");
    return 0;
}
