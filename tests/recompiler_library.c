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
    /* the program as a buffer at its guest address */
    check(!snx_recompiler_set_keep_ir(r, keep_ir), "set_keep_ir");
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
    if (keep_ir) {
        char* path = snx_path_join(keep_ir, backend[0] == 'l' ? "code_000.ll" : "code_000.c");
        check(snx_is_file(path), "keep_ir saved the first code unit");
        free(path);
        path = snx_path_join(keep_ir, backend[0] == 'l' ? "module.ll" : "module.c");
        check(snx_is_file(path), "keep_ir saved the descriptor");
        free(path);
    }
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
    int done, flagged;
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
    __atomic_store_n((uint32_t*)(s->mem + SPIN_FLAG), 1u, __ATOMIC_RELEASE);
    for (ms = 0; ms < 2000 && !__atomic_load_n(&s->done, __ATOMIC_ACQUIRE); ms += 10) nap(10);
    if (!__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)) {
        s->flagged = 1;
        __atomic_store_n(&s->cpu->poll, 1u, __ATOMIC_RELEASE);
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
    __atomic_store_n(&s.done, 1, __ATOMIC_RELEASE);
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

/* The order of the code units: the heaviest first, by instructions plus four per leader. entry calls wide, 4501
   instructions in one block, then branchy, 2001 instructions in about 2000 blocks (cbz x1,.+8; add x0,x0,#1, a
   thousand times). With 4 jobs the units are {entry, wide} and {branchy}: branchy weighs about 10000 to wide's 4500,
   so it is unit 0 (code_000), though it has fewer instructions and comes later in the code */
#define SCHEDULE64   0x100000u
#define WIDE_WORDS   4501u
#define BRANCH_PAIRS 1000u
/* whether the unit's saved IR defines the guest function at address */
static int unit_defines(const char* folder, const char* unit, uint64_t address) {
    char *path = snx_path_join(folder, unit), symbol[40];
    size_t size = 0;
    char* text = path ? (char*)snx_read_file(path, &size) : NULL;
    int found = 0;
    snprintf(symbol, sizeof symbol, "@F_%llX(", (unsigned long long)address);
    for (size_t at = 0; text && at < size;) {
        char* line = text + at;
        char* end = memchr(line, '\n', size - at);
        size_t length = end ? (size_t)(end - line) : size - at;
        if (length > 7 && !strncmp(line, "define ", 7)) {
            char* hit = strstr(line, symbol);
            found |= hit && hit < line + length;
        }
        at += length + 1;
    }
    free(text);
    free(path);
    return found;
}
static void schedule64(struct snx_recompiler* r) {
    const uint64_t wide = SCHEDULE64 + 3 * 4, branchy = wide + WIDE_WORDS * 4;
    const size_t words = 3 + WIDE_WORDS + 2 * BRANCH_PAIRS + 1;
    uint32_t* code = (uint32_t*)calloc(words, sizeof *code);
    uint8_t* bytes = (uint8_t*)calloc(words, 4);
    char* folder = snx_temp_dir(NULL);
    check(code && bytes && folder, "schedule: memory and a folder");
    if (!code || !bytes || !folder) {
        free(code);
        free(bytes);
        free(folder);
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
    snx_recompiler_set_jobs(r, 4);
    check(!snx_recompiler_set_keep_ir(r, folder), "schedule: set_keep_ir");
    struct snx_recompiled* m = snx_recompile(r, SCHEDULE64, bytes, n * 4);
    check(m != NULL && snx_recompiled_functions(m) == 3, "schedule: entry, wide and branchy found");
    check(unit_defines(folder, "code_000.ll", branchy) && !unit_defines(folder, "code_000.ll", wide),
          "schedule: the branchy function's unit, the heavier, is unit 0");
    check(unit_defines(folder, "code_001.ll", wide) && unit_defines(folder, "code_001.ll", SCHEDULE64),
          "schedule: entry and the wide function are unit 1");
    snx_recompiled_free(m);
    snx_recompiler_set_keep_ir(r, NULL);
    snx_recompiler_set_jobs(r, 2);
    snx_remove_tree(folder);
    free(folder);
    free(bytes);
    free(code);
}

int main(int argc, char** argv) {
    uint8_t* mem = (uint8_t*)calloc(MEMORY, 1);
    struct snx_recompiler* r;
    char *keep_ir, *cache;
    int verbose = argc > 1 && !strcmp(argv[1], "--verbose");
    if (!mem) return 2;
    place(mem, CODE64, a64_words, sizeof(a64_words) / sizeof(a64_words[0]));
    place(mem, CODE32, a32_words, sizeof(a32_words) / sizeof(a32_words[0]));
    check(snx_recompiler_abi() == SNX_ABI, "the library's ABI is this cpu.h's");
    check(!snx_recompiler_has(0) && !snx_recompiler_new(0), "flags must name a backend");
    check(!snx_recompiler_has(2) && !snx_recompiler_new(2), "flags must name LLVM");
    check(snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM), "LLVM is always built in");
    keep_ir = snx_temp_dir(NULL);
    check(keep_ir != NULL, "a folder for the IR");
    cache = snx_temp_dir(NULL);
    check(cache != NULL, "a folder for the caches");
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
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
        run64(r, "llvm", mem, keep_ir);
        progress64(r, mem);
        schedule64(r);
        invalidate64(r, mem);
#ifndef _WIN32
        spin64(r, mem);
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
