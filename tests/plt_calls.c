/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game module's calls through its PLT (loader.h's SnxBoundBranch): an extracted game of two modules, recompiled by
   the CLI, whose main calls the SDK module's ext_add, its own own_add and two, which both modules define, each
   through a PLT stub, and ends in a tail call of the SDK's ext_tail through a fourth. A stub whose symbol one module
   alone defines must call that function directly once its slot holds the function's address, whatever its dispatch
   slot holds, and go through CPU.disp while the slot holds anything else: the address the game's linker leaves there
   until the stub's first call, or another function. Two's stub, a stub of a function a hook names and every stub of a
   recompile with --hook-direct-calls must call through the table. The call is made where the BL or B is (the IR of
   the calling function calls the three functions itself), and goes to the stub's own function once the guest has
   changed the stub's code.
     plt_calls SCARLETNX DATA */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/dl.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/loader.h"
#include "nx/loader/module_abi.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif

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
static void put64(unsigned char* p, size_t off, unsigned long long n) {
    put32(p, off, (unsigned long)(n & 0xFFFFFFFFu));
    put32(p, off + 4, (unsigned long)(n >> 32));
}

/* a module's symbol: defined at value (a function) when value is not 0, else imported */
typedef struct {
    const char* name;
    unsigned value;
} Symbol;
/* MOD0 at 0x1000 of an image and its dynamic table at 0x1100: the symbols at 0x1200 (the first is the table's
   second: index 1), their names at 0x12a8 and, with slots, a JUMP_SLOT at 0x2000 + 8 i for the symbol of index
   slot[i], in a JMPREL table at 0x1400 */
static void dynamic(unsigned char* image, const Symbol* symbols, unsigned count, const unsigned* slot, unsigned slots) {
    size_t at = 0x1100, name = 1;
    unsigned i;
    put32(image, 4, 0x1000);
    memcpy(image + 0x1000, "MOD0", 4);
    put32(image, 0x1004, 0x100);
    put32(image, 0x1008, 0x2000); /* an empty BSS at 0x3000 */
    put32(image, 0x100c, 0x2000);
    put64(image, at, 6), put64(image, at + 8, 0x1200), at += 16; /* DT_SYMTAB */
    put64(image, at, 5), put64(image, at + 8, 0x12a8), at += 16; /* DT_STRTAB: seven symbols before it */
    put64(image, at, 10), put64(image, at + 8, 0x158), at += 16; /* DT_STRSZ */
    if (slots) {
        put64(image, at, 23), put64(image, at + 8, 0x1400), at += 16; /* DT_JMPREL */
        put64(image, at, 2), put64(image, at + 8, 24ull * slots);     /* DT_PLTRELSZ */
    }
    for (i = 0; i < slots; ++i) {
        put64(image, 0x1400 + 24 * i, 0x2000 + 8 * i);
        put64(image, 0x1408 + 24 * i, ((unsigned long long)slot[i] << 32) | 1026);
    }
    for (i = 0; i < count; ++i) {
        size_t s = 0x1200 + 24 * (i + 1), n = strlen(symbols[i].name) + 1;
        put32(image, s, (unsigned long)name);
        image[s + 4] = 0x12; /* a global function */
        image[s + 6] = symbols[i].value ? 1 : 0;
        put64(image, s + 8, symbols[i].value);
        memcpy(image + 0x12a8 + name, symbols[i].name, n);
        name += n;
    }
}
/* an NSO of text, rodata and data of a page each, stored as they are, and a page of BSS: image, its three pages */
static size_t nso(unsigned char* file, const unsigned char* image, unsigned char id) {
    unsigned i;
    memset(file, 0, 0x100);
    memcpy(file, "NSO0", 4);
    put32(file, 0x3c, 0x1000);
    for (i = 0; i < 3; ++i) {
        put32(file, 0x10 + i * 0x10, 0x100 + i * 0x1000);
        put32(file, 0x14 + i * 0x10, i * 0x1000);
        put32(file, 0x18 + i * 0x10, 0x1000);
        put32(file, 0x60 + i * 4, 0x1000);
    }
    memset(file + 0x40, id, 32);
    memcpy(file + 0x100, image, 0x3000);
    return 0x3100;
}

/* the offset field of a BL or B at from that goes to to */
static unsigned long rel(unsigned from, unsigned to) { return ((to - from) / 4) & 0x3FFFFFFul; }

/* where the game's functions lie in their modules */
enum {
    CALLER = 0x40,
    OTHER = 0x80,
    OWN_ADD = 0x100,
    MAIN_TWO = 0x120,
    STUB = 0x200, /* the first of four, 16 bytes each: of ext_add, own_add, two and ext_tail */
    EXT_ADD = 0x40,
    SDK_TWO = 0x60,
    EXT_TAIL = 0x80
};
/* The main module: game_main (CALLER) calls the stubs of ext_add, own_add and two and ends in a branch to
   ext_tail's, which another function (OTHER, never run) calls too and a symbol names (tail_stub), so that the branch
   leaves game_main; own_add copies x16 to x22 and adds 0x20 to x0, and its two adds 0x200. ext: the name ext_add
   goes by */
static void main_image(unsigned char* image, const char* ext) {
    const Symbol symbols[6] = {{"game_main", CALLER}, {"own_add", OWN_ADD},    {"two", MAIN_TWO}, {ext, 0},
                               {"ext_tail", 0},       {"tail_stub", STUB + 48}};
    const unsigned slot[4] = {4, 2, 3, 5}; /* the stubs' symbols: ext_add, own_add, two, ext_tail */
    unsigned i;
    memset(image, 0, 0x3000);
    put32(image, 0, 0x14000004);                                                /* b +16 */
    put32(image, 0x10, 0xD65F03C0);                                             /* ret */
    put32(image, CALLER, 0xAA1E03F3);                                           /* mov x19, x30 */
    put32(image, CALLER + 4, 0x94000000 | rel(CALLER + 4, STUB));               /* bl ext_add's stub */
    put32(image, CALLER + 8, 0x94000000 | rel(CALLER + 8, STUB + 16));          /* bl own_add's */
    put32(image, CALLER + 12, 0x94000000 | rel(CALLER + 12, STUB + 32));        /* bl two's */
    put32(image, CALLER + 16, 0xAA1303FE);                                      /* mov x30, x19 */
    put32(image, CALLER + 20, 0x14000000 | rel(CALLER + 20, STUB + 48));        /* b ext_tail's */
    put32(image, OTHER, 0x94000000 | rel(OTHER, STUB + 48));                    /* bl ext_tail's */
    put32(image, OTHER + 4, 0xD65F03C0);                                        /* ret */
    put32(image, OWN_ADD, 0xAA1003F6);                                          /* mov x22, x16 */
    put32(image, OWN_ADD + 4, 0x91008000), put32(image, OWN_ADD + 8, 0xD65F03C0); /* add x0, x0, #0x20; ret */
    put32(image, MAIN_TWO, 0x91080000), put32(image, MAIN_TWO + 4, 0xD65F03C0); /* add x0, x0, #0x200; ret */
    for (i = 0; i < 4; ++i) {
        put32(image, STUB + 16 * i, 0xD0000010);                       /* adrp x16, 0x2000 */
        put32(image, STUB + 16 * i + 4, 0xF9400211 | (i << 10));       /* ldr x17, [x16, #8 i] */
        put32(image, STUB + 16 * i + 8, 0x91000210 | ((8 * i) << 10)); /* add x16, x16, #8 i */
        put32(image, STUB + 16 * i + 12, 0xD61F0220);                  /* br x17 */
    }
    dynamic(image, symbols, 6, slot, 4);
}
/* The SDK module: ext_add copies x16 and x17 to x20 and x21 and adds 5 to x0, its two adds 0x400 and ext_tail 3 */
static void sdk_image(unsigned char* image, const char* ext) {
    const Symbol symbols[3] = {{ext, EXT_ADD}, {"two", SDK_TWO}, {"ext_tail", EXT_TAIL}};
    memset(image, 0, 0x3000);
    put32(image, 0, 0x14000004);
    put32(image, 0x10, 0xD65F03C0);
    put32(image, EXT_ADD, 0xAA1003F4), put32(image, EXT_ADD + 4, 0xAA1103F5);      /* mov x20, x16; mov x21, x17 */
    put32(image, EXT_ADD + 8, 0x91001400), put32(image, EXT_ADD + 12, 0xD65F03C0); /* add x0, x0, #5; ret */
    put32(image, SDK_TWO, 0x91100000), put32(image, SDK_TWO + 4, 0xD65F03C0);   /* add x0, x0, #0x400; ret */
    put32(image, EXT_TAIL, 0x91000C00), put32(image, EXT_TAIL + 4, 0xD65F03C0); /* add x0, x0, #3; ret */
    dynamic(image, symbols, 3, NULL, 0);
}

/* Whether the function the recompile dumped the IR of (the setting ir_dump_pc, in the recompile's output text) calls
   the function at callee itself */
static int ir_calls(const char* text, unsigned long long callee) {
    const char *from = text ? strstr(text, "IR before promotion:") : NULL, *hit;
    const char* to = from ? strstr(from, "IR after promotion:") : NULL;
    char call[40];
    snprintf(call, sizeof call, "@F_%llX(", callee);
    hit = from ? strstr(from, call) : NULL;
    return hit && to && hit < to;
}

/* snx_run, its output with every line ended as this file writes them: Windows' text streams end a line with CR LF */
static int run_cli(const char* const* args, const char* root, char** text) {
    int rc = snx_run(args, root, NULL, text);
    char *from, *to;
    for (from = to = *text; from && *from; ++from)
        if (*from != '\r') *to++ = *from;
    if (to) *to = 0;
    return rc;
}

/* What stands in for the runtime while the module's code runs: an address the dispatch table has no code for is a
   function that adds 1000 to x0 and returns, so a call that went through the table to such an address shows in x0 */
#define UNBOUND 0x7000
static int missed;
static void miss(CPU* c, u64 pc) {
    (void)pc;
    ++missed;
    c->x[0] += 1000;
    c->pc = c->x[30];
}
/* what the runtime puts in a function's dispatch slot, a hook's handler: it adds 7, and only a call through the
   table runs it */
static void override_add(CPU* c, u64 pc) {
    (void)pc;
    c->x[0] += 7;
    c->pc = c->x[30];
}
static void poll(CPU* c) { c->poll = 0; }
static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c, (void)pc, (void)w;
    check(0, "no undefined instruction");
}
static void svc(CPU* c, u32 imm) {
    (void)c, (void)imm;
    check(0, "no SVC");
}

/* how the game module at path calls through its stubs: BOUND, the three stubs of functions one module defines call
   them directly and two's goes through the table; ROUTED, as BOUND but ext_add, which a hook names, is called
   through the table; TABLE, every stub goes through the table */
enum { BOUND, ROUTED, TABLE };
static void module_runs(const char* path, const unsigned char* main_bytes, const unsigned char* sdk_bytes, int how) {
    typedef const SnxModule* (*ModuleFn)(void);
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    void *library = plat_dl_open(path), *sym;
    ModuleFn get;
    const SnxModule* m;
    GuestFn entry = NULL;
    unsigned char* memory;
    u64 a, b;
    u32 i;
    int step;
    if (!library) fprintf(stderr, "%s: %s\n", path, plat_dl_error());
    check(library != NULL, "the game module loads");
    if (!library) return;
    sym = plat_dl_sym(library, "snx_module");
    check(sym != NULL, "the game module has a descriptor");
    if (!sym) return;
    memcpy(&get, &sym, sizeof get);
    m = get();
    check(m->abi == SNX_ABI && m->nmods == 2, "the module is the game's two modules', of this ABI");
    if (m->nmods != 2) return;
    a = m->mods[0].base;
    b = m->mods[1].base;
    for (i = 0; i < m->nentries; ++i)
        if (m->entry_addr[i] == a + CALLER) entry = m->entry_fn[i];
    check(entry != NULL, "the function that calls through the stubs is an entry");
    memory = (unsigned char*)calloc(1, (size_t)b + 0x4000);
    /* 0: every slot as the game's linker leaves it until the stub's first call, an address with no function of the
       game's. 1: each bound to its function (two to the main module's, the first the linker finds), and a handler in
       each function's dispatch slot. 2: ext_add's slot holds another function, ext_tail */
    for (step = 0; entry && memory && step < 3; ++step) {
        CPU c = {0};
        u64 slot[4], want;
        unsigned direct = how == TABLE ? 0 : how == ROUTED ? 2 : 3;
        slot[0] = step == 1 ? b + EXT_ADD : step == 2 ? b + EXT_TAIL : UNBOUND;
        slot[1] = step ? a + OWN_ADD : UNBOUND;
        slot[2] = step ? a + MAIN_TWO : UNBOUND;
        slot[3] = step ? b + EXT_TAIL : UNBOUND;
        memcpy(memory + a, main_bytes, 0x3000);
        memcpy(memory + b, sdk_bytes, 0x3000);
        for (i = 0; i < 32; ++i) memory[a + 0x2000 + i] = (unsigned char)(slot[i / 8] >> (8 * (i % 8)));
        rt.svc = svc;
        rt.undef = undefined;
        rt.poll = poll;
        rt.miss = miss;
        c.rt = &rt;
        c.mem = memory;
        c.disp = (GuestFn*)calloc((size_t)(m->code_hi - m->code_lo) / 4, sizeof(GuestFn));
        c.code_lo = m->code_lo;
        c.code_hi = m->code_hi;
        for (i = 0; i < m->nentries; ++i) c.disp[(m->entry_addr[i] - m->code_lo) / 4] = m->entry_fn[i];
        if (step == 1) {
            c.disp[(b + EXT_ADD - m->code_lo) / 4] = override_add;
            c.disp[(a + OWN_ADD - m->code_lo) / 4] = override_add;
            c.disp[(a + MAIN_TWO - m->code_lo) / 4] = override_add;
            c.disp[(b + EXT_TAIL - m->code_lo) / 4] = override_add;
        }
        c.x[30] = 0x1000;
        missed = 0;
        entry(&c, a + CALLER);
        if (!step)
            want = 4000; /* four calls of an address with no code */
        else if (step == 1)
            /* directly: ext_add's 5 (the handler's 7 when a hook names it), own_add's 0x20 and ext_tail's 3; through
               the table: two, and all four when every call goes through it */
            want = direct == 3 ? 5 + 0x20 + 7 + 3 : direct == 2 ? 7 + 0x20 + 7 + 3 : 28;
        else
            want = 3 + 0x20 + 0x200 + 3; /* ext_tail's 3 where ext_add's 5 was bound */
        if (c.x[0] != want)
            fprintf(stderr, "how %d step %d: x0 = %llx, where %llx was wanted\n", how, step, (unsigned long long)c.x[0],
                    (unsigned long long)want);
        check(c.x[0] == want, !step       ? "a stub whose slot is unbound calls what the slot holds, through the table"
                              : step == 1 ? "a bound stub calls its function directly, two's through the table"
                                          : "a slot that holds another function calls that one");
        check(missed == (step ? 0 : 4), "only a call of an address with no code reaches the miss");
        check(c.pc == 0x1000, "it returns to its caller");
        /* what the last stub run leaves: its slot's address and what the slot held */
        check(c.x[16] == a + 0x2018 && c.x[17] == slot[3], "x16 and x17 are as the stub leaves them");
        if (how == BOUND && step == 1) /* ext_add and own_add ran, each called where its BL is */
            check(c.x[20] == a + 0x2000 && c.x[21] == b + EXT_ADD && c.x[22] == a + 0x2008,
                  "a function called directly finds x16 and x17 as its stub leaves them");
        free(c.disp);
    }
    /* a stub entered as a function, as a pointer to it reaches it, with its slot bound: its own branch calls the
       function directly too */
    if (memory && how == BOUND) {
        CPU c = {0};
        GuestFn stub = NULL;
        u64 slot = b + EXT_ADD;
        for (i = 0; i < m->nentries; ++i)
            if (m->entry_addr[i] == a + STUB) stub = m->entry_fn[i];
        check(stub != NULL, "the stub is an entry");
        memcpy(memory + a, main_bytes, 0x3000);
        memcpy(memory + b, sdk_bytes, 0x3000);
        for (i = 0; i < 8; ++i) memory[a + 0x2000 + i] = (unsigned char)(slot >> (8 * i));
        rt.svc = svc;
        rt.undef = undefined;
        rt.poll = poll;
        rt.miss = miss;
        c.rt = &rt;
        c.mem = memory;
        c.disp = (GuestFn*)calloc((size_t)(m->code_hi - m->code_lo) / 4, sizeof(GuestFn));
        c.code_lo = m->code_lo;
        c.code_hi = m->code_hi;
        for (i = 0; i < m->nentries; ++i) c.disp[(m->entry_addr[i] - m->code_lo) / 4] = m->entry_fn[i];
        c.disp[(b + EXT_ADD - m->code_lo) / 4] = override_add;
        c.x[30] = 0x1000;
        missed = 0;
        if (stub) stub(&c, a + STUB);
        check(c.x[0] == 5 && c.pc == 0x1000 && !missed, "a stub entered as a function calls its function directly");
        free(c.disp);
    }
    /* the guest changed the first stub's code (the runtime marks the function compiled from it stale): the call goes
       to that function, which leaves for the code there now, with the return address set and nothing else run */
    if (entry && memory && how == BOUND) {
        CPU c = {0};
        u64 slot[4] = {b + EXT_ADD, a + OWN_ADD, a + MAIN_TWO, b + EXT_TAIL};
        u32 stub = m->nfns;
        for (i = 0; i < m->nfns; ++i)
            if (m->fn_lo[i] == a + STUB) stub = i;
        check(stub < m->nfns && m->fn_len[stub] == 16, "the stub is a function of the module");
        memcpy(memory + a, main_bytes, 0x3000);
        memcpy(memory + b, sdk_bytes, 0x3000);
        for (i = 0; i < 32; ++i) memory[a + 0x2000 + i] = (unsigned char)(slot[i / 8] >> (8 * (i % 8)));
        rt.svc = svc;
        rt.undef = undefined;
        rt.poll = poll;
        rt.miss = miss;
        c.rt = &rt;
        c.mem = memory;
        c.disp = (GuestFn*)calloc((size_t)(m->code_hi - m->code_lo) / 4, sizeof(GuestFn));
        c.code_lo = m->code_lo;
        c.code_hi = m->code_hi;
        for (i = 0; i < m->nentries; ++i) c.disp[(m->entry_addr[i] - m->code_lo) / 4] = m->entry_fn[i];
        c.x[30] = 0x1000;
        missed = 0;
        if (stub < m->nfns) {
            m->stale[stub] = 1;
            entry(&c, a + CALLER);
            m->stale[stub] = 0;
            if (c.pc != a + STUB || c.x[0] || c.x[30] != a + CALLER + 8)
                fprintf(stderr, "stale stub: pc = %llx, x0 = %llx, x30 = %llx\n", (unsigned long long)c.pc,
                        (unsigned long long)c.x[0], (unsigned long long)c.x[30]);
            check(c.pc == a + STUB && !c.x[0] && c.x[30] == a + CALLER + 8 && !missed,
                  "a call of a stub whose code changed leaves for the stub's address");
        }
        free(c.disp);
    }
    free(memory);
    plat_dl_close(library);
}

int main(int argc, char** argv) {
    static unsigned char file[0x3100], main_bytes[0x3000], sdk_bytes[0x3000];
    char *root, *game, *exefs, *path, *out, *module, *text = NULL;
    const char *args[16], *hooked = NULL;
    char dump[40];
    const char* bound_line =
        "PLT: 4 calls through 3 stubs made direct, 1 stubs of their module's own functions and 2 of "
        "another module's\n";
    size_t r;
    int k, pass;
    if (argc != 3) return 2;
    snprintf(dump, sizeof dump, "--ir_dump_pc=%llx", (unsigned long long)SNX_CODE_BASE + CALLER);
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    game = snx_path_join(root, "Game"); /* the name its folder gives it */
    exefs = snx_path_join(game, "exefs");
    out = snx_path_join(root, "out");
    module = snx_path_join(out, "Game/Game_code" SUFFIX);
    check(!snx_mkdirs(exefs), "the game's folder");
    /* the shortest symbol of the route list: a name a hook of the runtime's names */
    for (r = 0; r < snx_hook_route_count(); ++r) {
        const char* candidate = snx_hook_route_name(r);
        if (strchr(candidate, ':') || (hooked && strlen(candidate) >= strlen(hooked))) continue;
        hooked = candidate;
    }
    check(hooked && strlen(hooked) < 100 && snx_hook_routed(hooked), "the route list names a symbol");
    /* 0: as it is. 1: ext_add by a name a hook names. 2: with every call through its dispatch slot */
    for (pass = 0; pass < 3; ++pass) {
        const char* ext = pass == 1 && hooked ? hooked : "ext_add";
        main_image(main_bytes, ext);
        sdk_image(sdk_bytes, ext);
        path = snx_path_join(exefs, "main");
        check(!snx_write_file(path, file, nso(file, main_bytes, 0x4e)), "write the game's main module");
        free(path);
        path = snx_path_join(exefs, "sdk");
        check(!snx_write_file(path, file, nso(file, sdk_bytes, 0x5d)), "write the game's SDK module");
        free(path);
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--data";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = game;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--nce=0"; /* code made ahead of time, which an NCE recompile has none of */
        if (pass == 2) args[k++] = "--hook-direct-calls";
        if (!pass) args[k++] = dump; /* the IR of the function that calls through the stubs, in the output */
        args[k] = NULL;
        if (run_cli(args, root, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            check(0, "the game recompiles");
        }
        if (pass < 2 && (!text || !strstr(text, bound_line)))
            fprintf(stderr, "wanted: %s in:\n%s\n", bound_line, text ? text : "");
        check(text && (pass == 2 ? !strstr(text, "PLT: ") : strstr(text, bound_line) != NULL),
              pass == 2 ? "nothing is bound when every call goes through its dispatch slot"
                        : "the recompile says how many calls through the PLT it made direct");
        if (!pass) { /* the call is made where the BL or B is, not in the stub's function */
            const SnxU64 a = SNX_CODE_BASE, b = SNX_CODE_BASE + 0x4000;
            check(ir_calls(text, a + CALLER), "the recompile prints the IR of the function that calls");
            check(ir_calls(text, b + EXT_ADD) && ir_calls(text, a + OWN_ADD) && ir_calls(text, b + EXT_TAIL),
                  "the calling function calls the three bound functions itself");
            check(!ir_calls(text, a + MAIN_TWO) && !ir_calls(text, b + SDK_TWO),
                  "and neither function of the symbol two modules define");
            /* its last branch too, which keeps the call of the stub for a slot that holds another address */
            check(ir_calls(text, a + STUB + 48), "the branch to a stub is run in the stub's place");
        }
        free(text);
        text = NULL;
        module_runs(module, main_bytes, sdk_bytes, pass == 2 ? TABLE : pass ? ROUTED : BOUND);
    }
    check(!snx_remove_tree(root), "fixture cleanup");
    free(module);
    free(out);
    free(exefs);
    free(game);
    free(root);
    printf("PLT calls: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
