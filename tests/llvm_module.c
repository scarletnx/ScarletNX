/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A whole program through the LLVM backend: a synthetic NRO with a loop, BL/RET, an indirect branch inside its
   function (the D_ dispatch), a tail call into another function, SVC, exclusives, FP and SIMD, recompiled by the CLI
   in small chunks (calls cross units). The module's run must end in the state qemu-aarch64 -cpu cortex-a57 ends the
   same program in from the same start (tests/qemu_ref, a free run of the image, its final RET to x30 = 0 taken by
   hand): every register, the flags, FP state, the stack memory and the SVC call. A miss (RtAPI.miss) stands in for
   the runtime JIT compiling the code at its pc: the module's function there, made live again, or the image's words
   as LLVM steps where no function is (the BR's target).
   CPU.poll is set when each run starts: the AOT code must call rt->poll at the loop's backward branch, resuming at
   its head. A second LLVM module built with --hook-direct-calls must run the same, with main's BL to func reaching
   func through CPU.disp, where a wrapper counts it. With func's functions stale (SnxModule.stale, as the runtime
   marks them when the guest patches their code: SNX_ABI 14) and out of CPU.disp, main's direct BL must leave the AOT
   code at func's entry for rt->miss, and the run end the same. With main made stale while it runs (from rt->poll,
   as a mod patches its caller from inside a call), the next call that returns into main must leave its AOT code
   too, for the dispatch loop the runtime's threads run, and end the same.
     llvm_module SCARLETNX REPO */
#include "llvm_steps.h"
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/loader/module_abi.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#include <elf.h>
#endif

/* assembled with clang -target aarch64-linux-gnu: main at 0x80 (reached by the B at 0), func, tail at 0x200 */
static const u32 program[] = {0xD2800000, 0xD2800181, 0x1E6F1001, 0x6F00E402, 0x94000016, 0x9E620003, 0x1E632800,
                              0x1E610800, 0x4EA28442, 0x4E040C24, 0x4EA48442, 0xF1000421, 0x54FFFF01, 0x10000062,
                              0xD61F0040, 0xD4200020, 0xF81F0FE0, 0xA8C113E3, 0xB4000043, 0x910FA000, 0x885FFFE7,
                              0x8808FFE0, 0x37180040, 0xD2401C00, 0xAA1F03FE, 0x14000047, 0x8B010000, 0xD37FF804,
                              0xCA440C00, 0x9B011005, 0x9AC108A6, 0x9A868000, 0xD65F03C0};
static const u32 tail[] = {0xAA0003E6, 0xD40000E1, 0x9E780009, 0xD65F03C0};

#define IMAGE  0x400
static u32 g_image_words[IMAGE / 4];
#define MEMORY 0x10000

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}

typedef struct {
    u64 imm[8], x0[8];
    int count, undefined;
} Calls;
static Calls* calls;
static void svc(CPU* c, u32 imm) {
    if (calls->count < 8) {
        calls->imm[calls->count] = imm;
        calls->x0[calls->count++] = c->x[0];
    }
}
static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++calls->undefined;
}

typedef struct {
    CPU cpu;
    unsigned char memory[MEMORY];
    Calls calls;
    int polls; /* rt->poll calls, and c->pc at the first */
    u64 poll_pc;
    int watched; /* calls through the dispatch entry run() wrapped */
    int stale, stale_misses; /* functions run() made stale, and rt->miss calls at the pc it made them stale at */
    int stale_on_poll, left;  /* main made stale at the first poll; returns to run()'s loop before the end */
    int misses, stepped, lost; /* rt->miss calls, those the steps ran, and those nothing could run */
    u64 base;                 /* the image's */
} Run;
static Run* current;
static GuestFn watched_entry;
static void watch(CPU* c, u64 e) {
    ++current->watched;
    watched_entry(c, e);
}
static u64 g_stale_pc;
/* what jit_code_changed does to the functions covering pc: out of dispatch first, then the byte. Their number */
static const SnxModule* g_running;
static int make_stale(CPU* c, u64 pc) {
    u64 slots = (g_running->code_hi - g_running->code_lo) / 4, k;
    u32 f;
    int n = 0;
    for (f = 0; f < g_running->nfns; ++f)
        if (pc >= g_running->fn_lo[f] && pc - g_running->fn_lo[f] < g_running->fn_len[f]) {
            for (k = 0; k < slots; ++k)
                if (c->disp[k] == g_running->fn[f]) c->disp[k] = NULL;
            g_running->stale[f] = 1;
            ++n;
        }
    return n;
}
/* the runtime takes the thread off its core here, then gives it back: once, the flag cleared */
static void on_poll(CPU* c) {
    if (!current->polls++) {
        current->poll_pc = c->pc;
        if (current->stale_on_poll) current->stale += make_stale(c, g_running->mods[0].base + 0x80);
    }
    c->poll = 0;
}

/* the image's words as LLVM steps at their pcs (llvm_steps.c), built at the first miss that needs them: what runs
   code no function of the module covers, as the runtime JIT would compile it (the BR's target, which no analysis
   finds) */
static struct {
    LlvmSteps steps;
    int ready;
    u64 base;
} g_stepper;

static int stepper_build(u64 base) {
    char error[1024];
    u32 i;
    if (g_stepper.ready) return g_stepper.base == base ? 0 : -1;
    if (llvm_steps_begin(&g_stepper.steps, 1, 0, error, sizeof(error))) return snx_error("%s", error);
    for (i = 0; i < IMAGE / 4; ++i) llvm_steps_add(&g_stepper.steps, g_image_words[i], base + 4 * i, 0);
    if (llvm_steps_load(&g_stepper.steps, error, sizeof(error))) return snx_error("%s", error);
    g_stepper.ready = 1;
    g_stepper.base = base;
    return 0;
}

/* RtAPI.miss: the runtime JIT would compile the code at pc, from bytes that are as they were. Inside a function of
   the module, that function made live again (its stale byte cleared, its slots of CPU.disp back) stands in for it,
   entered at pc as a dispatch entry would be. Elsewhere the steps run from pc until the code reaches a function's
   entry, which they hand on to, as the JIT's code would call or jump to it, or leaves the image (a return) */
static void miss(CPU* c, u64 pc) {
    const SnxModule* mod = g_running;
    const u64 base = mod->mods[0].base;
    u32 f, i, n;
    ++current->misses;
    if (pc == g_stale_pc) ++current->stale_misses;
    for (f = 0; f < mod->nfns; ++f)
        if (pc >= mod->fn_lo[f] && pc - mod->fn_lo[f] < mod->fn_len[f]) break;
    if (f < mod->nfns) {
        mod->stale[f] = 0;
        for (i = 0; i < mod->nentries; ++i)
            if (mod->entry_addr[i] >= mod->fn_lo[f] && mod->entry_addr[i] - mod->fn_lo[f] < mod->fn_len[f])
                c->disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
        if (current->misses > 100) {
            ++current->lost;
            c->pc = 0;
            return;
        }
        mod->fn[f](c, pc);
        return;
    }
    if (stepper_build(base)) {
        ++current->lost;
        c->pc = 0;
        return;
    }
    ++current->stepped;
    for (n = 0; n < 10000 && pc - base < IMAGE; ++n) {
        GuestFn entry = pc >= mod->code_lo && pc < mod->code_hi ? c->disp[(pc - mod->code_lo) / 4] : NULL;
        if (entry && n) {
            entry(c, pc);
            return;
        }
        pc = g_stepper.steps.steps[(pc - base) / 4](c);
    }
    if (n == 10000) ++current->lost;
    c->pc = pc;
}

/* watch_at: an offset in the image whose dispatch entry counts its calls (0: none) */
static int run(const char* path, const unsigned char* image, u64 watch_at, u64 stale_at, int stale_on_poll, Run* out) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    RtAPI rt;
    unsigned char* memory;
    size_t i;
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    if (!get) return -1;
    mod = get();
    g_running = mod;
    memset(out, 0, sizeof(*out));
    out->stale_on_poll = stale_on_poll;
    memset(&rt, 0, sizeof(rt));
    rt.svc = svc;
    rt.undef = undefined;
    rt.poll = on_poll;
    rt.miss = miss; /* a pc a module has no function for */
    calls = &out->calls;
    current = out;
    memory = (unsigned char*)calloc(1, (size_t)mod->mods[0].base + MEMORY);
    out->cpu.disp = (GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(GuestFn));
    memcpy(memory + mod->mods[0].base, image, IMAGE);
    for (i = 0; i < mod->nentries; ++i) out->cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    if (watch_at) {
        GuestFn* slot = &out->cpu.disp[(mod->mods[0].base + watch_at - mod->code_lo) / 4];
        watched_entry = *slot;
        if (watched_entry) *slot = watch;
    }
    if (stale_at) {
        out->stale = make_stale(&out->cpu, mod->mods[0].base + stale_at);
        g_stale_pc = mod->mods[0].base + stale_at;
    } else {
        g_stale_pc = ~0ull;
    }
    out->cpu.mem = memory;
    out->cpu.rt = &rt;
    out->cpu.code_lo = mod->code_lo;
    out->cpu.code_hi = mod->code_hi;
    out->cpu.x[31] = mod->mods[0].base + MEMORY / 2;
    out->cpu.x[30] = 0xDEAD0000;
    out->cpu.poll = 1;
    out->base = mod->mods[0].base;
    if (out->cpu.disp[0]) {
        int steps;
        out->cpu.disp[0](&out->cpu, mod->mods[0].base);
        /* the runtime's thread loop: what the code left at c->pc runs through dispatch, or rt->miss */
        for (steps = 0; out->cpu.pc && out->cpu.pc != 0xDEAD0000 && steps < 1000; ++steps) {
            u64 pc = out->cpu.pc;
            GuestFn f = pc >= mod->code_lo && pc < mod->code_hi ? out->cpu.disp[(pc - mod->code_lo) / 4] : NULL;
            ++out->left;
            (f ? f : rt.miss)(&out->cpu, pc);
        }
    }
    memcpy(out->memory, memory + mod->mods[0].base, MEMORY);
    if (stale_at || stale_on_poll) memset(mod->stale, 0, mod->nfns); /* the module's own data, should it stay loaded */
    free(memory);
    free(out->cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return 0;
}

static int same(const Run* a, const Run* b) {
    return !memcmp(a->cpu.x, b->cpu.x, sizeof(a->cpu.x)) && a->cpu.pc == b->cpu.pc && a->cpu.n == b->cpu.n &&
           a->cpu.z == b->cpu.z && a->cpu.cf == b->cpu.cf && a->cpu.vf == b->cpu.vf &&
           !memcmp(a->cpu.v, b->cpu.v, sizeof(a->cpu.v)) && a->cpu.fpsr == b->cpu.fpsr &&
           a->cpu.excl_on == b->cpu.excl_on && !memcmp(a->memory, b->memory, MEMORY) &&
           !memcmp(&a->calls, &b->calls, sizeof(a->calls));
}

/* what same() found apart, with both values, for a log from a machine the test can't be rerun on */
static void differences(const Run* a, const Run* b, const char* what) {
    size_t i;
    for (i = 0; i < sizeof(a->cpu.x) / sizeof(a->cpu.x[0]); ++i)
        if (a->cpu.x[i] != b->cpu.x[i])
            fprintf(stderr, "  %s x%lu=%llx, llvm %llx\n", what, (unsigned long)i, (unsigned long long)b->cpu.x[i],
                    (unsigned long long)a->cpu.x[i]);
    for (i = 0; i < sizeof(a->cpu.v) / sizeof(a->cpu.v[0]); ++i)
        if (memcmp(&a->cpu.v[i], &b->cpu.v[i], sizeof(a->cpu.v[i])))
            fprintf(stderr, "  %s v%lu=%llx:%llx, llvm %llx:%llx\n", what, (unsigned long)i,
                    (unsigned long long)b->cpu.v[i].d[1], (unsigned long long)b->cpu.v[i].d[0],
                    (unsigned long long)a->cpu.v[i].d[1], (unsigned long long)a->cpu.v[i].d[0]);
    if (a->cpu.pc != b->cpu.pc || a->cpu.n != b->cpu.n || a->cpu.z != b->cpu.z || a->cpu.cf != b->cpu.cf ||
        a->cpu.vf != b->cpu.vf || a->cpu.fpsr != b->cpu.fpsr || a->cpu.excl_on != b->cpu.excl_on)
        fprintf(stderr, "  %s pc=%llx nzcv=%u%u%u%u fpsr=%x excl=%u, llvm pc=%llx nzcv=%u%u%u%u fpsr=%x excl=%u\n",
                what, (unsigned long long)b->cpu.pc, b->cpu.n, b->cpu.z, b->cpu.cf, b->cpu.vf, (unsigned)b->cpu.fpsr,
                b->cpu.excl_on, (unsigned long long)a->cpu.pc, a->cpu.n, a->cpu.z, a->cpu.cf, a->cpu.vf,
                (unsigned)a->cpu.fpsr, a->cpu.excl_on);
    for (i = 0; i < MEMORY; ++i)
        if (a->memory[i] != b->memory[i]) {
            fprintf(stderr, "  %s memory differs from offset %lx\n", what, (unsigned long)i);
            break;
        }
    if (memcmp(&a->calls, &b->calls, sizeof(a->calls))) fprintf(stderr, "  %s calls differ\n", what);
}

/* the end state qemu-aarch64 -cpu cortex-a57 gives the program from run()'s start (SP base + MEMORY / 2, X30
   0xDEAD0000, the rest zero, the image in zeroed memory), a free run of the image under tests/qemu_ref's runner with
   the window and the code at base; the final RET, to the X30 = 0 that main's tail sets, by hand */
static void expected(Run* want, const unsigned char* image, u64 base) {
    static const struct {
        unsigned n;
        u64 value;
    } x[] = {{0, 0x49A}, {3, 0x7D}, {5, 0x145}, {6, 0x49A}, {9, 0x3BB4}};
    static const struct {
        unsigned n;
        u64 lo, hi;
    } v[] = {{0, 0x40CDDA0518000000ull, 0},
             {1, 0x3FF8000000000000ull, 0},
             {2, 0x0000B0010000B001ull, 0x0000B0010000B001ull},
             {3, 0x405F400000000000ull, 0},
             {4, 0x0000000100000001ull, 0x0000000100000001ull}};
    const u64 sp = base + MEMORY / 2;
    unsigned i;
    memset(want, 0, sizeof(*want));
    for (i = 0; i < sizeof(x) / sizeof(x[0]); ++i) want->cpu.x[x[i].n] = x[i].value;
    want->cpu.x[2] = base + 0xC0; /* ADR X2 of the BR's target */
    want->cpu.x[31] = sp;
    for (i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        want->cpu.v[v[i].n].d[0] = SNX_GUEST64(v[i].lo);
        want->cpu.v[v[i].n].d[1] = SNX_GUEST64(v[i].hi);
    }
    want->cpu.z = want->cpu.cf = 1; /* NZCV 0110 from the loop's last SUBS */
    want->cpu.fpsr = 0x10;          /* IXC */
    want->cpu.pc = 0;
    /* the image, X0 (0x7D) stored at SP - 16 by the pre-indexed STR, W0 (0x465) by the STLXR at SP, which succeeds */
    memcpy(want->memory, image, IMAGE);
    put32(want->memory, MEMORY / 2 - 16, 0x7D);
    put32(want->memory, MEMORY / 2, 0x465);
    want->calls.count = 1;
    want->calls.imm[0] = 7;
    want->calls.x0[0] = 0x49A;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/* the address of a symbol of the module's symbol table, 0 for none */
static u64 symbol_address(const unsigned char* elf, size_t size, const char* name) {
    Elf64_Ehdr eh;
    if (size < sizeof(eh)) return 0;
    memcpy(&eh, elf, sizeof(eh));
    for (size_t i = 0; i < eh.e_shnum && eh.e_shoff + (i + 1) * sizeof(Elf64_Shdr) <= size; ++i) {
        Elf64_Shdr sh, strings;
        memcpy(&sh, elf + eh.e_shoff + i * sizeof(sh), sizeof(sh));
        if (sh.sh_type != SHT_SYMTAB || sh.sh_link >= eh.e_shnum) continue;
        memcpy(&strings, elf + eh.e_shoff + sh.sh_link * sizeof(strings), sizeof(strings));
        for (size_t j = 0; (j + 1) * sizeof(Elf64_Sym) <= sh.sh_size && sh.sh_offset + sh.sh_size <= size; ++j) {
            Elf64_Sym sym;
            memcpy(&sym, elf + sh.sh_offset + j * sizeof(sym), sizeof(sym));
            if (sym.st_name < strings.sh_size && !strcmp((const char*)elf + strings.sh_offset + sym.st_name, name))
                return sym.st_value;
        }
    }
    return 0;
}
/* the address of the module's function for the guest's at address */
static u64 function_address(const unsigned char* elf, size_t size, u64 address) {
    char name[32];
    sprintf(name, "F_%llX", (unsigned long long)address);
    return symbol_address(elf, size, name);
}

/* --layout: the functions of its file go first in the module's code, in the file's order, and the code runs the
   same. func (0xE8) first, then the image's entry (0, with main, which its B reaches), before the runtime library */
static void layout(const char* scarletnx, const char* repo, const char* root, const char* input, const char* self,
                   const unsigned char* image, const Run* plain) {
    char *dir = snx_path_join(root, "layout"), *file = snx_path_join(root, "order.layout"), *text = NULL;
    char *module = snx_path_join(dir, "Program/Program_code.so"), *order;
    const char* args[16] = {scarletnx,   "--repo", repo,      "-r", input,      "Program", "--outdir", dir,
                            "--runtime", self,     "--chunk", "4",  "--layout", file,      NULL};
    unsigned char* elf;
    size_t size = 0;
    u64 func_at, entry_at, library_at;
    static Run laid;
    order = snx_format("%llx\n0x%llX\n", (unsigned long long)(plain->base + 0xE8), (unsigned long long)plain->base);
    check(!snx_write_file(file, order, strlen(order)), "write the layout file");
    if (snx_run(args, root, NULL, &text)) fprintf(stderr, "%s\n", text ? text : "");
    check(text && strstr(text, "layout: 2 functions first, in the order of 2 addresses"), "a build with --layout");
    elf = snx_read_file(module, &size);
    func_at = elf ? function_address(elf, size, plain->base + 0xE8) : 0;
    entry_at = elf ? function_address(elf, size, plain->base) : 0;
    /* the runtime library's first function, where the module's code starts without an order */
    library_at = elf ? symbol_address(elf, size, "snx_fp_compare") : 0;
    if (!(func_at && func_at < entry_at && entry_at < library_at))
        fprintf(stderr, "func %llx, entry %llx, runtime library %llx\n", (unsigned long long)func_at,
                (unsigned long long)entry_at, (unsigned long long)library_at);
    check(func_at && func_at < entry_at && entry_at < library_at,
          "--layout's functions start the module's code, in its order");
    check(!run(module, image, 0, 0, 0, &laid), "the run of the laid-out module");
    if (!same(plain, &laid)) differences(plain, &laid, "layout");
    check(same(plain, &laid), "the laid-out module runs the same");
    free(elf);
    free(text);
    free(order);
    free(module);
    free(file);
    free(dir);
}
#endif

int main(int argc, char** argv) {
    static Run runs[1], want, hooked, unhooked, stale, stale_running;
    unsigned char image[IMAGE];
    char *root, *input, *self, *out[2], *module[2], *text = NULL;
    const char* args[24];
    int b, k;
    size_t i;
    if (argc != 3) return 2;
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    memset(image, 0, sizeof(image));
    put32(image, 0, 0x14000020); /* B main */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, IMAGE);
    put32(image, 0x24, 0x300);
    put32(image, 0x28, 0x300);
    put32(image, 0x2c, 0x80);
    put32(image, 0x30, 0x380);
    put32(image, 0x34, 0x80);
    for (i = 0; i < sizeof(program) / sizeof(program[0]); ++i) put32(image, 0x80 + 4 * i, program[i]);
    for (i = 0; i < sizeof(tail) / sizeof(tail[0]); ++i) put32(image, 0x200 + 4 * i, tail[i]);
    for (i = 0; i < IMAGE / 4; ++i)
        g_image_words[i] = (u32)image[4 * i] | (u32)image[4 * i + 1] << 8 | (u32)image[4 * i + 2] << 16 |
                           (u32)image[4 * i + 3] << 24;
    input = snx_path_join(root, "program.nro");
    self = snx_executable(argv[0]);
    check(!snx_write_file(input, image, sizeof(image)), "write NRO");
    for (b = 0; b < 2; ++b) { /* the second: with --hook-direct-calls */
        out[b] = snx_path_join(root, b ? "llvm-hooked" : "llvm");
#ifdef _WIN32
        module[b] = snx_path_join(out[b], "Program/Program_code.dll");
#else
        module[b] = snx_path_join(out[b], "Program/Program_code.so");
#endif
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Program";
        args[k++] = "--outdir";
        args[k++] = out[b];
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = "--chunk";
        args[k++] = "4";
        if (b) args[k++] = "--hook-direct-calls";
        args[k++] = "-q";
        args[k] = NULL;
        if (snx_run(args, root, NULL, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            fprintf(stderr, "%s build failed\n", b ? "hooked llvm" : "llvm");
            check(0, "build");
        }
        free(text);
        text = NULL;
        if (b) break;
        check(!run(module[0], image, 0, 0, 0, &runs[0]), "AOT run");
    }
    /* func, which main's BL at 0x90 calls on each pass of the loop */
    check(!run(module[0], image, 0xE8, 0, 0, &unhooked) && !run(module[1], image, 0xE8, 0, 0, &hooked),
          "hooked and unhooked runs");
    check(!run(module[0], image, 0, 0xE8, 0, &stale), "a run with func stale");
    check(!run(module[0], image, 0, 0, 1, &stale_running), "a run with main made stale while it runs");
    check(stale_running.stale > 0 && stale_running.left > 0,
          "a call returning into a function made stale while it ran leaves its AOT code for dispatch");
    if (!same(&runs[0], &stale_running)) differences(&runs[0], &stale_running, "stale while running");
    check(same(&runs[0], &stale_running), "the run with main made stale while it runs is the same");
    check(runs[0].left == 0, "an ordinary run never leaves its AOT code early");
    check(runs[0].misses == 1 && runs[0].stepped == 1, "the BR's target, no function's code, is the run's one miss");
    check(!runs[0].lost && !stale.lost && !stale_running.lost && !hooked.lost && !unhooked.lost,
          "every miss runs");
    check(stale.stale > 0 && stale.stale_misses > 0, "a stale function's direct caller reaches it through rt->miss");
    if (!same(&runs[0], &stale)) differences(&runs[0], &stale, "stale");
    check(same(&runs[0], &stale), "the run with func stale is the same");
    check(same(&runs[0], &hooked), "the --hook-direct-calls module runs the same");
    check(!unhooked.watched && hooked.watched > 0,
          "a direct BL reaches its function through CPU.disp only with --hook-direct-calls");
    check(runs[0].cpu.pc == 0 && runs[0].calls.count == 1 && runs[0].calls.imm[0] == 7 && !runs[0].calls.undefined,
          "the program returns to 0 after one SVC #7");
    expected(&want, image, runs[0].base);
    if (!same(&runs[0], &want)) differences(&runs[0], &want, "QEMU");
    check(same(&runs[0], &want), "LLVM AOT ends as QEMU does");
    /* the loop: b.ne back to main+0x10 */
    if (runs[0].polls != 1 || runs[0].poll_pc != runs[0].cpu.code_lo + 0x90)
        fprintf(stderr, "%d polls, the first at %llx\n", runs[0].polls, (unsigned long long)runs[0].poll_pc);
    check(runs[0].polls == 1 && runs[0].poll_pc == runs[0].cpu.code_lo + 0x90,
          "AOT code polls at the loop's backward branch");
    printf("x0=%llx x9=%llx d0=%llx v2=%llx:%llx\n", (unsigned long long)runs[0].cpu.x[0],
           (unsigned long long)runs[0].cpu.x[9], (unsigned long long)runs[0].cpu.v[0].d[0],
           (unsigned long long)runs[0].cpu.v[2].d[1], (unsigned long long)runs[0].cpu.v[2].d[0]);
#if !defined(_WIN32) && !defined(__APPLE__)
    layout(argv[1], argv[2], root, input, self, image, &runs[0]);
#endif
    /* the setting unit_instructions sizes the code units as --chunk does: one instruction makes a unit of each
       function, a million one unit of them all */
    for (b = 0; b < 2; ++b) {
        char* dir = snx_path_join(root, b ? "units-whole" : "units-each");
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Program";
        args[k++] = "--outdir";
        args[k++] = dir;
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = b ? "--unit_instructions=1000000" : "--unit_instructions=1";
        args[k] = NULL;
        check(!snx_run(args, root, NULL, &text), "a build sized by unit_instructions");
        if (text && !strstr(text, b ? "LLVM: 1 code unit and" : "code units and")) fprintf(stderr, "%s\n", text);
        check(text && strstr(text, b ? "LLVM: 1 code unit and" : "code units and"),
              b ? "a million instructions: one code unit" : "one instruction: a code unit of each function");
        free(text);
        text = NULL;
        free(dir);
    }
    if (g_stepper.ready) llvm_steps_end(&g_stepper.steps);
    check(!snx_remove_tree(root), "fixture cleanup");
    for (b = 0; b < 2; ++b) {
        free(out[b]);
        free(module[b]);
    }
    free(input);
    free(self);
    free(root);
    printf("LLVM module: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
