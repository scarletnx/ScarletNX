/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NROs a game maps while it runs, made ahead of time (driver/nros.c): an extracted game whose RomFS holds two
   NROs (one of them twice, under two names), a file that is no NRO and an NRO whose image is not whole pages,
   recompiled by the CLI. The game module must tell where each NRO goes (snx_nros: in the order of their SHA-256, one
   after another behind the game's own module, a guard page before each), and each NRO must have its own module,
   <hash>.so in <Game>_nro, that names the NRO at that address and whose code runs as code at that address does (an ADR
   gives it). A module an earlier recompile left for an NRO the game has no more is removed, and nothing else in the
   folder. A second recompile without one of the NROs leaves one place and one module.
     nro_modules SCARLETNX DATA */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/dl.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/loader.h"
#include "nx/loader/module_abi.h"
#include <openssl/evp.h>
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
/* MOD0 at 0x1000 of an image and its dynamic table at 0x1100: a symbol table of two symbols at 0x1200 (the first a
   function the image defines at value; the second, when named, an import), their names at 0x1260, and, with slots, a
   JUMP_SLOT of the second symbol at 0x2000 and of the first at 0x2008 */
static void dynamic(unsigned char* image, const char* first, unsigned long long value, const char* second, int slots) {
    size_t a = strlen(first) + 1, at = 0x1100;
    put32(image, 4, 0x1000);
    memcpy(image + 0x1000, "MOD0", 4);
    put32(image, 0x1004, 0x100);
    put32(image, 0x1008, 0x2000); /* an empty BSS at 0x3000 */
    put32(image, 0x100c, 0x2000);
    put64(image, at, 6), put64(image, at + 8, 0x1200), at += 16; /* DT_SYMTAB */
    put64(image, at, 5), put64(image, at + 8, 0x1260), at += 16; /* DT_STRTAB */
    put64(image, at, 10), put64(image, at + 8, 0x80), at += 16;  /* DT_STRSZ */
    if (slots) {
        put64(image, at, 23), put64(image, at + 8, 0x1300), at += 16; /* DT_JMPREL */
        put64(image, at, 2), put64(image, at + 8, 48);                /* DT_PLTRELSZ */
        put64(image, 0x1300, 0x2000), put64(image, 0x1308, (2ull << 32) | 1026);
        put64(image, 0x1318, 0x2008), put64(image, 0x1320, (1ull << 32) | 1026);
    }
    put32(image, 0x1218, 1), image[0x121c] = 0x12, image[0x121e] = 1, put64(image, 0x1220, value);
    memcpy(image + 0x1261, first, a);
    if (second) {
        put32(image, 0x1230, (unsigned long)(1 + a)), image[0x1234] = 0x12;
        memcpy(image + 0x1261 + a, second, strlen(second) + 1);
    }
}
/* the game's one module: text, rodata and data of a page each, stored as they are, a page of BSS. It exports one
   function at 0x40, which adds 5 to x0, by the name g_game_symbol: game_add, then a symbol a hook of the runtime's
   names */
#define GAME_ADD (SNX_CODE_BASE + 0x40)
static const char* g_game_symbol = "game_add";
static size_t nso(unsigned char* file) {
    unsigned i;
    memset(file, 0, 0x3100);
    memcpy(file, "NSO0", 4);
    put32(file, 0x3c, 0x1000);
    for (i = 0; i < 3; ++i) {
        put32(file, 0x10 + i * 0x10, 0x100 + i * 0x1000);
        put32(file, 0x14 + i * 0x10, i * 0x1000);
        put32(file, 0x18 + i * 0x10, 0x1000);
        put32(file, 0x60 + i * 4, 0x1000);
    }
    memset(file + 0x40, 0x4e, 32);
    put32(file, 0x100, 0x14000004); /* b +16 */
    put32(file, 0x110, 0xD65F03C0); /* ret */
    put32(file, 0x140, 0x91001400); /* game_add: add x0, x0, #5 */
    put32(file, 0x144, 0xD65F03C0); /* ret */
    dynamic(file + 0x100, g_game_symbol, 0x40, NULL, 0);
    return 0x3100;
}
/* an NRO of three pages and bss bytes of BSS, its module ID id thirty-two times: 0 branches to 0x80, which leaves
   0x84 of its own address plus add in x0 and returns. With calls, it first calls the game's game_add and its own
   nro_own (0x100: x0 += 0x20), each through a PLT stub (0x200, 0x210) whose slot (0x2000, 0x2008) the game's linker
   fills while the game runs */
static void nro(unsigned char* image, unsigned char id, unsigned add, unsigned long bss, int calls) {
    memset(image, 0, 0x3000);
    put32(image, 0, 0x14000020); /* b 0x80 */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, 0x3000);
    put32(image, 0x24, 0x1000);
    put32(image, 0x28, 0x1000);
    put32(image, 0x2c, 0x1000);
    put32(image, 0x30, 0x2000);
    put32(image, 0x34, 0x1000);
    put32(image, 0x38, bss);
    memset(image + 0x40, id, 32);
    put32(image, 0x80, 0xAA1E03F3);               /* mov x19, x30 */
    put32(image, 0x84, 0x10000000);               /* adr x0, . */
    put32(image, 0x88, 0x91000000 | (add << 10)); /* add x0, x0, #add */
    put32(image, 0x8c, 0xD503201F);               /* nop */
    put32(image, 0x90, 0xD503201F);               /* nop */
    put32(image, 0x94, 0xAA1303FE);               /* mov x30, x19 */
    put32(image, 0x98, 0xD65F03C0);               /* ret */
    if (!calls) return;
    put32(image, 0x8c, 0x94000000 | ((0x200 - 0x8c) >> 2));           /* bl 0x200 */
    put32(image, 0x90, 0x94000000 | ((0x210 - 0x90) >> 2));           /* bl 0x210 */
    put32(image, 0x100, 0x91008000);                                  /* nro_own: add x0, x0, #0x20 */
    put32(image, 0x104, 0xD65F03C0);                                  /* ret */
    put32(image, 0x200, 0xD0000010), put32(image, 0x204, 0xF9400211); /* adrp x16, 0x2000; ldr x17, [x16] */
    put32(image, 0x208, 0x91000210), put32(image, 0x20c, 0xD61F0220); /* add x16, x16, #0; br x17 */
    put32(image, 0x210, 0xD0000010), put32(image, 0x214, 0xF9400611); /* adrp x16, 0x2000; ldr x17, [x16, #8] */
    put32(image, 0x218, 0x91002210), put32(image, 0x21c, 0xD61F0220); /* add x16, x16, #8; br x17 */
    dynamic(image, "nro_own", 0x100, g_game_symbol, 1);
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
static void hex(const unsigned char* hash, char* out) {
    int i;
    for (i = 0; i < 32; ++i) sprintf(out + 2 * i, "%02x", hash[i]);
}

/* What stands in for the runtime while an NRO's module runs: the return to x30, outside the NRO, ends the run; any
   other address the dispatch table has no code for is a function that adds 1000 to x0 and returns, so a call that
   went through the table shows in x0. game_add is the game module's function, as an import bound to it calls it */
#define RETURN 0x1000
static int missed;
static void miss(CPU* c, u64 pc) {
    ++missed;
    if (pc == RETURN) {
        c->pc = pc;
        return;
    }
    c->x[0] += 1000;
    c->pc = c->x[30];
}
static void game_add(CPU* c, u64 pc) {
    (void)pc;
    c->x[0] += 5;
    c->pc = c->x[30];
}
/* what the runtime puts in the game's function's place, a hook's handler or NVN's implementation: it adds 7 */
static void override_add(CPU* c, u64 pc) {
    if (pc != GAME_ADD) c->x[0] += 100000; /* it is called as the function at the address it answers */
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

/* The module at path is the NRO's of module ID id at base, size bytes with its BSS, and its code entered at the NRO's
   first instruction leaves base + 0x84 + add in x0. image, for an NRO with calls: the NRO, mapped at base for the run,
   whose two PLT slots are filled as the game's linker fills them. The module must import game_add alone, call it
   through CPU.disp until the import is bound and directly after, call its own nro_own directly, and take either call
   through CPU.disp when the slot holds another address than the one its symbol names */
static void module_runs(const char* path, u64 base, u64 size, unsigned char id, unsigned add,
                        const unsigned char* image) {
    typedef const SnxModule* (*ModuleFn)(void);
    typedef const SnxNroImports* (*ImportsFn)(void);
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    void *library = plat_dl_open(path), *sym;
    ModuleFn get;
    ImportsFn get_imports;
    const SnxModule* m;
    const SnxNroImports* imports = NULL;
    GuestFn entry = NULL, bound = game_add, override = override_add;
    unsigned char ids[32], *memory;
    u32 i;
    int step;
    if (!library) fprintf(stderr, "%s: %s\n", path, plat_dl_error());
    check(library != NULL, "the NRO's module loads");
    if (!library) return;
    sym = plat_dl_sym(library, "snx_module");
    check(sym != NULL, "the NRO's module has a descriptor");
    if (!sym) return;
    memcpy(&get, &sym, sizeof get);
    m = get();
    memset(ids, id, sizeof ids);
    check(m->abi == SNX_ABI && m->nmods == 1 && m->mods[0].is_nro, "the module is one NRO's, of this ABI");
    check(m->mods[0].base == base && m->mods[0].size == size, "the module is made for the NRO's place");
    check(!memcmp(m->mods[0].build_id, ids, 32), "the module names the NRO's module ID");
    check(m->code_lo == base && m->code_hi == base + 0x1000, "the module's code is the NRO's text");
    for (i = 0; i < m->nentries; ++i)
        if (m->entry_addr[i] == base) entry = m->entry_fn[i];
    check(entry != NULL, "the NRO's first instruction is an entry");
    sym = plat_dl_sym(library, "snx_imports");
    check((sym != NULL) == (image != NULL), "a module has imports when its NRO calls the game's functions");
    if (sym) {
        memcpy(&get_imports, &sym, sizeof get_imports);
        imports = get_imports();
        check(imports->version == SNX_NRO_IMPORTS_VERSION && imports->count == 1 && imports->address[0] == GAME_ADD,
              "the module imports the game's function its PLT names, and not its own");
        check(imports->function[0] == imports->dynamic && imports->dynamic, "an import starts as the table's lookup");
    }
    memory = (unsigned char*)calloc(1, (size_t)base + 0x4000);
    /* 0: as made. With calls, 1: the import bound; 2: the game's slot holds another function; 3: the NRO's own does;
       4: the import bound to the runtime's function in the game's place */
    for (step = 0; entry && memory && step < (image ? 5 : 1); ++step) {
        CPU c = {0};
        u64 want = base + 0x84 + add, slot[2];
        slot[0] = step == 2 ? 0x7000 : GAME_ADD;
        slot[1] = step == 3 ? 0x7000 : base + 0x100;
        if (image) {
            memcpy(memory + base, image, 0x3000);
            for (i = 0; i < 16; ++i) memory[base + 0x2000 + i] = (unsigned char)(slot[i / 8] >> (8 * (i % 8)));
            if (step) memcpy(&imports->function[0], step == 4 ? &override : &bound, sizeof bound);
            want += (step == 4 ? 7 : step == 1 || step == 3 ? 5 : 1000) + (step == 3 ? 1000 : 0x20);
        }
        rt.svc = svc;
        rt.undef = undefined;
        rt.poll = poll;
        rt.miss = miss;
        c.rt = &rt;
        c.mem = memory;
        c.disp = (GuestFn*)calloc(0x1000 / 4, sizeof(GuestFn));
        c.code_lo = base;
        c.code_hi = base + 0x1000;
        for (i = 0; i < m->nentries; ++i) c.disp[(m->entry_addr[i] - base) / 4] = m->entry_fn[i];
        c.x[30] = RETURN;
        missed = 0;
        entry(&c, base);
        if (c.x[0] != want)
            fprintf(stderr, "step %d: x0 = %llx, where %llx was wanted\n", step, (unsigned long long)c.x[0],
                    (unsigned long long)want);
        check(c.x[0] == want, !image      ? "the NRO's code runs as code at its place"
                              : !step     ? "an unbound import calls through the table"
                              : step == 1 ? "a bound import and the NRO's own function are called directly"
                              : step == 4 ? "an import bound to the runtime's function in the game's place calls it"
                                          : "a slot that holds another address goes through the table");
        /* through the table: the game's function unbound or replaced (steps 0, 2), the NRO's own replaced (3) */
        check(c.pc == RETURN && missed <= (image && step != 1 && step != 4 ? 2 : 1),
              "it returns to its caller, outside the NRO");
        if (image) check(missed >= (step == 1 || step == 4 ? 0 : 1), "a call through the table reaches the miss");
        free(c.disp);
    }
    free(memory);
    plat_dl_close(library);
}

int main(int argc, char** argv) {
    static unsigned char file[0x3100], a[0x3000], b[0x3000], odd[0x3000];
    unsigned char hash[2][32];
    char *root, *game, *exefs, *romfs, *sub, *path, *out, *folder, *module, *stale, *other, *text = NULL;
    char name[2][80], line[160];
    const char* args[16];
    const SnxNros* nros;
    void *library, *sym;
    const SnxNros* (*get)(void);
    int first, k, pass;
    if (argc != 3) return 2;
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    game = snx_path_join(root, "Game"); /* the name its folder gives it */
    exefs = snx_path_join(game, "exefs");
    romfs = snx_path_join(game, "romfs");
    sub = snx_path_join(romfs, "nro/deep");
    check(!snx_mkdirs(exefs) && !snx_mkdirs(sub), "the game's folders");
    path = snx_path_join(exefs, "main");
    check(!snx_write_file(path, file, nso(file)), "write the game's module");
    free(path);
    nro(a, 0xA1, 1, 0x1000, 1);
    nro(b, 0xB2, 2, 0, 0);
    nro(odd, 0xC3, 3, 0x800, 0); /* a BSS that is not whole pages: ldr:ro maps no such NRO */
    path = snx_path_join(sub, "a.nro");
    check(!snx_write_file(path, a, sizeof a), "write an NRO");
    free(path);
    path = snx_path_join(romfs, "A-again.NRO");
    check(!snx_write_file(path, a, sizeof a), "write it again");
    free(path);
    path = snx_path_join(romfs, "b.nro");
    check(!snx_write_file(path, b, sizeof b), "write another NRO");
    free(path);
    path = snx_path_join(romfs, "odd.nro");
    check(!snx_write_file(path, odd, sizeof odd), "write an NRO of odd sizes");
    free(path);
    path = snx_path_join(romfs, "text.nro");
    check(!snx_write_file(path, "not one", 7), "write a file that is no NRO");
    free(path);
    check(EVP_Digest(a, sizeof a, hash[0], NULL, EVP_sha256(), NULL) &&
              EVP_Digest(b, sizeof b, hash[1], NULL, EVP_sha256(), NULL),
          "the NROs' hashes");
    first = memcmp(hash[0], hash[1], 32) < 0 ? 0 : 1; /* which of a and b the table has first */
    for (k = 0; k < 2; ++k) {
        hex(hash[k], name[k]);
        strcpy(name[k] + 64, SUFFIX);
    }
    out = snx_path_join(root, "out");
    folder = snx_path_join(out, "Game/Game" SNX_NROS_FOLDER);
    module = snx_path_join(out, "Game/Game_code" SUFFIX);
    /* what an earlier recompile left: the module of an NRO the game has no more, and a file that is none */
    check(!snx_mkdirs(folder), "the folder of an earlier recompile");
    memset(line, 'f', 64);
    strcpy(line + 64, SUFFIX);
    stale = snx_path_join(folder, line);
    other = snx_path_join(folder, "notes.txt");
    check(!snx_write_file(stale, "old", 3) && !snx_write_file(other, "kept", 4), "an earlier recompile's files");
    for (pass = 0; pass < 2; ++pass) {
        /* b's place when a is before it: behind a's four pages and a guard page */
        u64 lo = SNX_CODE_BASE + 0x4000, base[2];
        u32 count = pass ? 1 : 2;
        if (pass) { /* the game without b */
            path = snx_path_join(romfs, "b.nro");
            check(!snx_remove_tree(path), "remove an NRO");
            free(path);
            first = 0;
        }
        base[first] = lo + 0x1000;
        base[!first] = base[first] + (first ? 0x3000 : 0x4000) + 0x1000;
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--data";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = game;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--nce=0"; /* code made ahead of time, which an NCE recompile has none of */
        args[k] = NULL;
        if (run_cli(args, root, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            check(0, "the game recompiles");
        }
        snprintf(line, sizeof line, "NROs: %u in the RomFS (%u files), %u instructions of text, at 0x%llX-0x%llX\n",
                 (unsigned)count, pass ? 2u : 3u, (unsigned)count * 0x400u, (unsigned long long)lo + 0x1000,
                 (unsigned long long)(pass ? lo + 0x5000 : lo + 0x9000));
        if (!text || !strstr(text, line)) fprintf(stderr, "wanted: %s in:\n%s\n", line, text ? text : "");
        check(text && strstr(text, line), "the recompile says where the NROs go");
        check(text && strstr(text, "NRO text.nro: it is not an NRO: no code is made for it\n"),
              "a file that is no NRO is said and left out");
        check(text && strstr(text, "NRO odd.nro: its image or its BSS is not whole pages, which ldr:ro maps none of: "
                                   "no code is made for it\n"),
              "an NRO ldr:ro would not map is said and left out");
        snprintf(line, sizeof line, "  NRO %u/%u: ", (unsigned)count, (unsigned)count);
        check(text && strstr(text, line), "each NRO's module is said as it is made");
        check(text && strstr(text, "; 2 calls through 2 PLT stubs made direct, 1 stubs of the NROs' own functions and "
                                   "1 of the game's\n"),
              "the recompile says how many of the NROs' PLT calls it made direct");
        free(text);
        text = NULL;
        /* the game module's table */
        library = plat_dl_open(module);
        if (!library) fprintf(stderr, "%s: %s\n", module, plat_dl_error());
        check(library != NULL, "the game module loads");
        sym = library ? plat_dl_sym(library, "snx_nros") : NULL;
        check(sym != NULL, "the game module has the table of its NROs");
        if (sym) {
            memcpy(&get, &sym, sizeof get);
            nros = get();
            check(nros->version == SNX_NROS_VERSION && nros->count == count, "the table has each NRO once");
            check(nros->lo == lo && nros->hi == (pass ? lo + 0x5000 : lo + 0x9000),
                  "the NROs lie behind the game's module");
            if (nros->count == count) {
                check(!memcmp(nros->slots[0].hash, hash[first], 32) && nros->slots[0].base == base[first] &&
                          nros->slots[0].size == (first ? 0x3000u : 0x4000u),
                      "the first NRO by hash is first, a guard page behind the module");
                if (!pass)
                    check(!memcmp(nros->slots[1].hash, hash[!first], 32) && nros->slots[1].base == base[!first] &&
                              nros->slots[1].size == (first ? 0x4000u : 0x3000u),
                          "the second is a guard page behind the first");
                check(snx_nro_slot(nros, hash[first]) == 0 && snx_nro_slot(nros, hash[0]) == (first ? 1 : 0) &&
                          snx_nro_slot(nros, (const unsigned char*)line) == -1,
                      "an NRO is found in the table by its hash");
            }
        }
        if (library) plat_dl_close(library);
        /* each NRO's own module, and nothing of an earlier recompile's but what is no module */
        path = snx_path_join(folder, name[0]);
        module_runs(path, base[0], 0x4000, 0xA1, 1, a);
        free(path);
        path = snx_path_join(folder, name[1]);
        if (!pass)
            module_runs(path, base[1], 0x3000, 0xB2, 2, NULL);
        else
            check(!snx_path_exists(path), "the module of an NRO the game has no more is removed");
        free(path);
        check(!snx_path_exists(stale), "an earlier recompile's module is removed");
        check(snx_is_file(other), "a file that is no module stays");
    }
    /* The game's function by a name a hook of the runtime's names (the shortest symbol of the route list), which the
       game module calls through its dispatch slot: the NRO's stub of it is bound all the same, to the import the
       runtime gives the hook's handler */
    {
        const char* hooked = NULL;
        unsigned char renamed[32];
        size_t r;
        for (r = 0; r < snx_hook_route_count(); ++r) {
            const char* candidate = snx_hook_route_name(r);
            if (strchr(candidate, ':') || (hooked && strlen(candidate) >= strlen(hooked))) continue;
            hooked = candidate;
        }
        check(hooked && strlen(hooked) < 100 && snx_hook_routed(hooked), "the route list names a symbol");
        g_game_symbol = hooked ? hooked : "game_add";
        path = snx_path_join(exefs, "main");
        check(!snx_write_file(path, file, nso(file)), "write the game's module, its function by a hooked name");
        free(path);
        nro(a, 0xA1, 1, 0x1000, 1);
        path = snx_path_join(sub, "a.nro");
        check(!snx_write_file(path, a, sizeof a), "write the NRO that imports it");
        free(path);
        path = snx_path_join(romfs, "A-again.NRO");
        check(!snx_write_file(path, a, sizeof a), "write it again");
        free(path);
        check(EVP_Digest(a, sizeof a, renamed, NULL, EVP_sha256(), NULL), "its hash");
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--data";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = game;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--nce=0";
        args[k] = NULL;
        if (run_cli(args, root, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            check(0, "the game recompiles with a hooked function");
        }
        check(text && strstr(text, "; 2 calls through 2 PLT stubs made direct, 1 stubs of the NROs' own functions and "
                                   "1 of the game's\n"),
              "a stub of a function a hook names is bound");
        free(text);
        text = NULL;
        hex(renamed, line);
        strcpy(line + 64, SUFFIX);
        path = snx_path_join(folder, line);
        module_runs(path, SNX_CODE_BASE + 0x5000, 0x4000, 0xA1, 1, a);
        free(path);
    }
    /* with every call sent through its dispatch slot no stub is bound, and a function two of the game's modules
       export is no stub's: the game's linker picks between them */
    for (pass = 0; pass < 2; ++pass) {
        if (pass) {
            path = snx_path_join(exefs, "sdk");
            check(!snx_write_file(path, file, nso(file)), "write a second module of the game, with the same export");
            free(path);
        }
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--data";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = game;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--nce=0";
        if (!pass) args[k++] = "--hook-direct-calls";
        args[k] = NULL;
        if (run_cli(args, root, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            check(0, "the game recompiles again");
        }
        check(text && strstr(text, pass ? "; 1 calls through 1 PLT stubs made direct, 1 stubs of the NROs' own "
                                          "functions and 0 of the game's\n"
                                        : "; 0 calls through 0 PLT stubs made direct, 0 stubs of the NROs' own "
                                          "functions and 0 of the game's\n"),
              pass ? "a function two modules export is not bound" : "nothing is bound when every call is hooked");
        free(text);
        text = NULL;
    }
    check(!snx_remove_tree(root), "fixture cleanup");
    free(stale);
    free(other);
    free(module);
    free(folder);
    free(out);
    free(sub);
    free(romfs);
    free(exefs);
    free(game);
    free(root);
    printf("NRO modules: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
