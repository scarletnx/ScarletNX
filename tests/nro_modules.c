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

/* the game's one module: text, rodata and data of a page each, stored as they are, a page of BSS; no MOD0 */
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
    put32(file, 0x100, 0x14000002); /* b +8 */
    put32(file, 0x108, 0xD65F03C0); /* ret */
    return 0x3100;
}
/* an NRO of three pages and bss bytes of BSS, its module ID id thirty-two times: 0 branches to 0x80, which leaves its
   own address plus add in x0 and returns */
static void nro(unsigned char* image, unsigned char id, unsigned add, unsigned long bss) {
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
    put32(image, 0x80, 0x10000000);               /* adr x0, . */
    put32(image, 0x84, 0x91000000 | (add << 10)); /* add x0, x0, #add */
    put32(image, 0x88, 0xD65F03C0);               /* ret */
}
static void hex(const unsigned char* hash, char* out) {
    int i;
    for (i = 0; i < 32; ++i) sprintf(out + 2 * i, "%02x", hash[i]);
}

static int missed;
static void miss(CPU* c, u64 pc) { /* the return to x30, outside the NRO: the run's end */
    ++missed;
    c->pc = pc;
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

/* the module at path is the NRO's of module ID id at base, size bytes with its BSS, and its code entered at the
   NRO's first instruction leaves base + 0x80 + add in x0 */
static void module_runs(const char* path, u64 base, u64 size, unsigned char id, unsigned add) {
    typedef const SnxModule* (*ModuleFn)(void);
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    void *library = plat_dl_open(path), *sym;
    ModuleFn get;
    const SnxModule* m;
    GuestFn entry = NULL;
    unsigned char ids[32];
    u32 i;
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
    if (entry) {
        CPU c = {0};
        rt.svc = svc;
        rt.undef = undefined;
        rt.poll = poll;
        rt.miss = miss;
        c.rt = &rt;
        c.disp = (GuestFn*)calloc(0x1000 / 4, sizeof(GuestFn));
        c.code_lo = base;
        c.code_hi = base + 0x1000;
        for (i = 0; i < m->nentries; ++i) c.disp[(m->entry_addr[i] - base) / 4] = m->entry_fn[i];
        c.x[30] = 0x1000; /* outside the NRO */
        missed = 0;
        entry(&c, base);
        if (c.x[0] != base + 0x80 + add)
            fprintf(stderr, "x0 = %llx, where %llx + 0x80 + %u was wanted\n", (unsigned long long)c.x[0],
                    (unsigned long long)base, add);
        check(c.x[0] == base + 0x80 + add, "the NRO's code runs as code at its place");
        /* the RET leaves the module's code for the thread's loop, or reaches rt->miss itself: either way at x30 */
        check(missed <= 1 && c.pc == 0x1000, "it returns to its caller, outside the NRO");
        free(c.disp);
    }
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
    nro(a, 0xA1, 1, 0x1000);
    nro(b, 0xB2, 2, 0);
    nro(odd, 0xC3, 3, 0x800); /* a BSS that is not whole pages: ldr:ro maps no such NRO */
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
        if (snx_run(args, root, NULL, &text)) {
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
        module_runs(path, base[0], 0x4000, 0xA1, 1);
        free(path);
        path = snx_path_join(folder, name[1]);
        if (!pass)
            module_runs(path, base[1], 0x3000, 0xB2, 2);
        else
            check(!snx_path_exists(path), "the module of an NRO the game has no more is removed");
        free(path);
        check(!snx_path_exists(stale), "an earlier recompile's module is removed");
        check(snx_is_file(other), "a file that is no module stays");
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
