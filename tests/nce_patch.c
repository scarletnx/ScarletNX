/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's patcher on a synthetic module (src/nx/runtime/nce/nce_patch.c): each kind of word that Eden's patcher
   changes is found in the code, and the words of the same encodings that the code reads as data, or that lie outside
   every function, keep their bytes. Then the stubs: each patched word branches to its stub, the stub does its work in
   the words that the runtime's outcall expects, and comes back to the next word. Words only: any host runs this */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/loader/codegen_view.h"
#include "nx/runtime/nce/nce_patch.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

#define BASE SNX_CODE_BASE
#define AT(i) (BASE + (SnxU64)(i) * 4)
/* where Android puts the guest's code under NCE: 64 GiB up (mem.c) */
#define MOVED (64ull << 30)

/* the synthetic text, one word each:
   f0, an FDE from word 0 to 18 that ends in a literal pool and a table:
     0 SVC #0x26             1 MRS X1, TPIDRRO_EL0   2 MRS X2, TPIDR_EL0    3 MSR TPIDR_EL0, X3
     4 MRS X4, CNTPCT_EL0    5 MRS X5, CNTFRQ_EL0    6 LDXR X6, [X7]        7 STXR W8, X6, [X7]
     8 LDAXR X9, [X7]        9 MRS XZR, TPIDRRO_EL0 10 LDR X10, =word 14   11 ADR X11, word 16
    12 MRS X12, CNTVCT_EL0  13 RET
    14-15 the literal that word 10 loads: an SVC's and an MRS's encodings
    16-17 the table that word 11 takes: an SVC's and an MRS TPIDR_EL0's
   18 an SVC's encoding outside every function, 19 zero
   f1, a symbol at word 20: 20 B.EQ word 22, 21 SVC #3, 22 STXP W1, X2, X3, [X4], 23 RET
   f2, an FDE from word 24 to 28: 24 B word 26, 25 SVC #4 (no flow reaches it), 26 RET, 27 RET */
static const SnxU32 text[] = {
    0xD40004C1u, 0xD53BD061u, 0xD53BD042u, 0xD51BD043u, 0xD53BE024u, 0xD53BE005u, 0xC85F7CE6u, 0xC8087CE6u,
    0xC85FFCE9u, 0xD53BD07Fu, 0x5800008Au, 0x100000ABu, 0xD53BE04Cu, 0xD65F03C0u, 0xD40004C1u, 0xD53BD061u,
    0xD4000021u, 0xD53BD040u, 0xD4000041u, 0x00000000u, 0x54000040u, 0xD4000061u, 0xC8210C82u, 0xD65F03C0u,
    0x14000002u, 0xD4000081u, 0xD65F03C0u, 0xD65F03C0u,
};
#define WORDS (sizeof text / sizeof *text)

static const NceSite* site_at(const NceSites* s, SnxU64 a) {
    for (size_t i = 0; i < s->count; i++)
        if (s->site[i].addr == a) return &s->site[i];
    return NULL;
}

static void expect(const NceSites* s, unsigned word, NceKind kind, int proven, const char* what) {
    const NceSite* site = site_at(s, AT(word));
    char text_[160];
    snprintf(text_, sizeof text_, "word %u: %s", word, what);
    check(site && site->kind == kind && site->proven == proven, text_);
}

static void expect_none(const NceSites* s, unsigned word, const char* what) {
    char text_[160];
    snprintf(text_, sizeof text_, "word %u keeps its bytes: %s", word, what);
    check(!site_at(s, AT(word)), text_);
}

/* ---- decoding the stubs' words ---- */

static SnxU64 b_target(SnxU32 w, SnxU64 at) {
    SnxU64 imm = w & 0x3FFFFFFu;
    if (imm & 0x2000000u) imm |= ~(SnxU64)0 << 26;
    return at + (imm << 2);
}
static int is_b(SnxU32 w) { return (w & 0xFC000000u) == 0x14000000u; }

typedef struct {
    unsigned char buf[4096];
    NcePool pool;
} Pool;

static void pool_init(Pool* p, SnxU64 addr) {
    memset(p->buf, 0, sizeof p->buf);
    p->pool.buf = p->buf;
    p->pool.addr = addr;
    p->pool.size = sizeof p->buf;
    p->pool.used = 0;
}
static SnxU32 word(const Pool* p, SnxU64 a) { return snx_ld_le32(p->buf + (size_t)(a - p->pool.addr)); }

static void scan_test(NceSites* sites) {
    unsigned char image[WORDS * 4];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde fde[2];
    SnxU64 symbol = 20 * 4;
    for (size_t i = 0; i < WORDS; i++) snx_st_le32(image + i * 4, text[i]);
    memset(&program, 0, sizeof program);
    memset(&module, 0, sizeof module);
    module.name = "synthetic";
    module.image = image;
    module.image_size = sizeof image;
    module.base = BASE;
    module.size = 0x1000;
    module.text_size = (SnxU32)sizeof image;
    fde[0].start = 0;
    fde[0].size = 18 * 4;
    fde[1].start = 24 * 4;
    fde[1].size = 4 * 4;
    module.fde = fde;
    module.fde_count = 2;
    module.functions = &symbol;
    module.function_count = 1;
    program.modules = &module;
    program.module_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "the analysis of the synthetic module");
    nce_find_sites(&codegen, sites);
    snx_codegen_free(&codegen);

    expect(sites, 0, NCE_SVC, 1, "SVC in the flow");
    check(site_at(sites, AT(0)) && site_at(sites, AT(0))->imm == 0x26, "the SVC's number");
    expect(sites, 1, NCE_TPIDRRO, 1, "MRS TPIDRRO_EL0");
    expect(sites, 2, NCE_TPIDR, 1, "MRS TPIDR_EL0");
    expect(sites, 3, NCE_TPIDR_SET, 1, "MSR TPIDR_EL0");
    expect(sites, 4, NCE_TICKS, 1, "MRS CNTPCT_EL0");
    expect(sites, 5, NCE_FREQ, 1, "MRS CNTFRQ_EL0");
    expect(sites, 6, NCE_EXCLUSIVE, 1, "LDXR");
    expect(sites, 7, NCE_EXCLUSIVE, 1, "STXR");
    expect_none(sites, 8, "LDAXR is ordered already");
    expect_none(sites, 9, "MRS to XZR");
    expect(sites, 12, NCE_TICKS, 1, "MRS CNTVCT_EL0");
    expect_none(sites, 14, "a literal that a load reads, inside the FDE");
    expect_none(sites, 15, "the literal's second word");
    expect_none(sites, 16, "a table that an ADR takes, inside the FDE");
    expect_none(sites, 17, "the rest of the table");
    expect_none(sites, 18, "outside every function");
    expect(sites, 21, NCE_SVC, 1, "SVC on a branch's fall-through");
    expect(sites, 22, NCE_EXCLUSIVE, 1, "STXP");
    expect(sites, 25, NCE_SVC, 0, "an SVC inside an FDE that no flow reaches, which Eden would patch too");
    check(sites->proven == 11 && sites->unproven == 1, "eleven sites in the flow and one more in a function");
    check(sites->data == 4 && sites->outside == 1, "four words of data and one outside");
}

static void stub_test(const NceSites* sites) {
    static const NceEnv env = {0x1010, 0x7F0012345678ull, 0}; /* tls past a page, to take the ADD */
    Pool pool, far;
    SnxU32 patch;
    pool_init(&pool, BASE + 0x100000); /* after the module, as the runtime puts it */
    pool_init(&far, BASE + (200ull << 20));
    for (size_t i = 0; i < sites->count; i++) {
        const NceSite* s = &sites->site[i];
        SnxU64 stub = pool.pool.addr + pool.pool.used;
        NcePool both[2];
        char what[96];
        both[0] = far.pool; /* out of reach: the second pool takes it */
        both[1] = pool.pool;
        snprintf(what, sizeof what, "the stub of the site at word %u", (unsigned)((s->addr - BASE) / 4));
        check(!nce_emit(both, 2, &env, s, &patch), what);
        pool.pool = both[1];
        check(both[0].used == 0, "nothing in the pool out of reach");
        if (s->kind == NCE_EXCLUSIVE) {
            check(patch == (s->word | 0x8000u), "an exclusive made ordered, in place");
            continue;
        }
        check(is_b(patch) && b_target(patch, s->addr) == stub, "the site branches to its stub");
        switch ((NceKind)s->kind) {
        case NCE_SVC:
        case NCE_TICKS: {
            SnxU64 code = s->kind == NCE_SVC ? NCE_OUTCALL(NCE_SVC, s->imm) : NCE_OUTCALL(NCE_TICKS, s->reg);
            check(word(&pool, stub) == 0xD10083FFu && word(&pool, stub + 4) == 0xA90047F0u &&
                      word(&pool, stub + 8) == 0xF9000BFEu,
                  "the outcall keeps x16, x17 and x30 below the guest's sp");
            check(word(&pool, stub + 12) == (0xD2800010u | (SnxU32)(code & 0xFFFF) << 5) &&
                      word(&pool, stub + 16) == (0xF2A00010u | (SnxU32)((code >> 16) & 0xFFFF) << 5),
                  "the outcall's code in x16");
            check(word(&pool, stub + 20) == 0x580000F1u && word(&pool, stub + 24) == 0xD63F0220u,
                  "the runtime's address from the literal, called");
            check(snx_ld_le64(pool.buf + (size_t)(stub + 48 - pool.pool.addr)) == env.outcall,
                  "the literal is the outcall's address, 8-aligned");
            check(word(&pool, stub + 28) == 0xA94047F0u && word(&pool, stub + 32) == 0xF9400BFEu &&
                      word(&pool, stub + 36) == 0x910083FFu,
                  "the three registers back, sp as it was");
            check(b_target(word(&pool, stub + 40), stub + 40) == s->addr + 4, "back at the next word");
            break;
        }
        case NCE_TPIDRRO:
        case NCE_TPIDR: {
            SnxU32 t = s->reg, off = s->kind == NCE_TPIDRRO ? offsetof(CPU, tpidrro) : offsetof(CPU, tpidr);
            check(word(&pool, stub) == (0xD53BD040u | t), "the host's thread pointer");
            check(word(&pool, stub + 4) == (0x91400000u | 1u << 10 | t << 5 | t), "the TLS offset's page");
            check(word(&pool, stub + 8) == (0xF9400000u | (0x10u / 8) << 10 | t << 5 | t), "the thread's CPU");
            check(word(&pool, stub + 12) == (0xF9400000u | (off / 8) << 10 | t << 5 | t), "the CPU's register");
            check(b_target(word(&pool, stub + 16), stub + 16) == s->addr + 4, "back at the next word");
            break;
        }
        case NCE_TPIDR_SET:
            check(word(&pool, stub) == 0xF81F0FF0u && word(&pool, stub + 20) == 0xF84107F0u,
                  "x16 kept below the guest's sp, then back");
            check(word(&pool, stub + 16) == (0xF9000000u | (offsetof(CPU, tpidr) / 8) << 10 | 16u << 5 | s->reg),
                  "the register into CPU.tpidr");
            check(b_target(word(&pool, stub + 24), stub + 24) == s->addr + 4, "back at the next word");
            break;
        case NCE_FREQ:
            check(word(&pool, stub) == (0xD2800000u | (NCE_CNTFRQ & 0xFFFFu) << 5 | s->reg) &&
                      word(&pool, stub + 4) == (0xF2A00000u | (NCE_CNTFRQ >> 16) << 5 | s->reg),
                  "19.2 MHz");
            check(b_target(word(&pool, stub + 8), stub + 8) == s->addr + 4, "back at the next word");
            break;
        default: check(0, "a kind with no stub");
        }
    }
    {
        NceSite s = sites->site[0];
        NcePool only = far.pool;
        check(nce_emit(&only, 1, &env, &s, &patch) == -1, "no stub where no pool is in reach");
    }
}

/* the indirect form (Android's TLS_SLOT_APP): the word at tls holds a pointer to the thread's block, whose first word
   is the CPU pointer. The same three words, so the stubs keep their sizes; tls within an LDR's 32 KiB */
static void indirect_test(const NceSites* sites) {
    static const NceEnv env = {0x10, 0x7F0012345678ull, 1};
    NceEnv far = env;
    Pool pool;
    SnxU32 patch;
    unsigned tested = 0;
    pool_init(&pool, BASE + 0x100000);
    for (size_t i = 0; i < sites->count; i++) {
        const NceSite* s = &sites->site[i];
        SnxU64 stub = pool.pool.addr + pool.pool.used, used = pool.pool.used;
        SnxU32 t = s->reg, cpu;
        if (s->kind != NCE_TPIDR && s->kind != NCE_TPIDRRO && s->kind != NCE_TPIDR_SET) continue;
        check(!nce_emit(&pool.pool, 1, &env, s, &patch), "an indirect stub for each TLS site");
        check(pool.pool.used - used == nce_stub_size((NceKind)s->kind), "an indirect stub of the direct one's size");
        check(is_b(patch) && b_target(patch, s->addr) == stub, "the site branches to its indirect stub");
        if (s->kind == NCE_TPIDR_SET) {
            stub += 4; /* after the STR that keeps the scratch register */
            t = 16;
        }
        cpu = s->kind == NCE_TPIDR_SET ? 0 : 1;
        check(word(&pool, stub) == (0xD53BD040u | t), "the host's thread pointer");
        check(word(&pool, stub + 4) == (0xF9400000u | (0x10u / 8) << 10 | t << 5 | t),
              "the pointer in the word at tls");
        check(word(&pool, stub + 8) == (0xF9400000u | t << 5 | t), "the CPU pointer, the block's first word");
        if (cpu)
            check(word(&pool, stub + 12) ==
                      (0xF9400000u |
                       (SnxU32)((s->kind == NCE_TPIDRRO ? offsetof(CPU, tpidrro) : offsetof(CPU, tpidr)) / 8) << 10 |
                       t << 5 | t),
                  "the CPU's register");
        tested++;
    }
    check(tested == 3, "the three TLS sites of the synthetic module");
    far.tls = 0x8000;
    check(nce_emit(&pool.pool, 1, &far, &sites->site[1], &patch) == -1, "an indirect tls past an LDR's reach");
    far.tls = 0x7FF8;
    check(!nce_emit(&pool.pool, 1, &far, &sites->site[1], &patch), "an indirect tls at the LDR's last word");
    far.tls = 0x14;
    check(nce_emit(&pool.pool, 1, &far, &sites->site[1], &patch) == -1, "a tls that is not a multiple of 8");
}

/* which games NCE takes on a host that has it (nce_refused): on Linux each 64-bit one; on Android from API 29 (Android
   10), whose app owns TLS_SLOT_APP, and the 39-bit address space only, as the guest's addresses move above 64 GiB. The
   reason says the values */
static void refused_test(void) {
    char why[240] = "";
    check(!nce_refused(why, sizeof why, 3, 0, 0) && !nce_refused(why, sizeof why, 1, 0, 0), "Linux takes both spaces");
    check(!nce_refused(why, sizeof why, 3, 1, NCE_ANDROID_API), "Android 10 takes a 39-bit game");
    check(nce_refused(why, sizeof why, 3, 1, 28) == -1 && strstr(why, "level is 28") && strstr(why, "needs 29"),
          "Android 9 does not, and says its level and the one needed");
    check(nce_refused(why, sizeof why, 1, 1, 34) == -1 && strstr(why, "36-bit") && strstr(why, "64 GiB"),
          "nor a 36-bit game on Android, which says why");
}

static void divert_test(void) {
    static const NceEnv env = {0x10, 0x7F0012345678ull, 0};
    static const struct {
        SnxU32 first;
        int ok;
        const char* what;
    } cases[] = {
        {0xA9BF7BFDu, 1, "STP X29, X30, [SP, #-16]! moves as it is"},
        {0x90000010u, 1, "ADRP X16 becomes its address"},
        {0x14000040u, 1, "B moves to the stub"},
        {0x94000040u, 1, "BL moves to the stub, then on at entry + 4"},
        {0x54000040u, 0, "B.EQ cannot move"},
        {0xB4000040u, 0, "CBZ cannot move"},
        {0x58000040u, 0, "a literal load cannot move"},
    };
    SnxU64 entry = BASE + 0x2004;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Pool pool;
        SnxU32 patch = 0;
        SnxU64 original = 0, stub;
        pool_init(&pool, BASE + 0x100000);
        stub = pool.pool.addr;
        check((nce_divert(&pool.pool, 1, &env, entry, cases[i].first, 7, &patch, &original) == 0) == cases[i].ok,
              cases[i].what);
        if (!cases[i].ok) continue;
        check(is_b(patch) && b_target(patch, entry) == stub, "the entry branches to the hook's outcall");
        check(word(&pool, stub + 12) == (0xD2800010u | 7u << 5) &&
                  word(&pool, stub + 16) == (0xF2A00010u | (SnxU32)NCE_HOOK << 5),
              "the hook's index in x16");
        check(word(&pool, stub + 40) == 0xD61F0200u, "the hook goes on at the pc that the runtime gives");
        check(original == stub + 56, "the original after the outcall");
        if (i == 0) {
            check(word(&pool, original) == cases[i].first, "the first word as it is");
            check(b_target(word(&pool, original + 4), original + 4) == entry + 4, "then on at entry + 4");
        } else if (i == 1) {
            SnxU64 page = entry & ~(SnxU64)0xFFF;
            check(word(&pool, original) == (0xD2800010u | (SnxU32)(page & 0xFFFF) << 5) &&
                      word(&pool, original + 4) == (0xF2A00010u | (SnxU32)((page >> 16) & 0xFFFF) << 5),
                  "the page's address in X16");
            check(b_target(word(&pool, original + 16), original + 16) == entry + 4, "then on at entry + 4");
        } else if (i == 2) {
            check(b_target(word(&pool, original), original) == entry + 0x100, "the B's target, and nothing after");
        } else {
            check((word(&pool, original) & 0xFC000000u) == 0x94000000u &&
                      b_target(word(&pool, original), original) == entry + 0x100,
                  "the BL's target");
            check(b_target(word(&pool, original + 4), original + 4) == entry + 4, "then on at entry + 4");
        }
    }
}

/* the trap, the stub's way back, and the sites file of an NCE recompile with the moves of its modules */
static void trap_file_test(const NceSites* found, const char* dir) {
    static const NceEnv env = {0x10, 0x7F0012345678ull, 0};
    static const NceRange ranges[2] = {{BASE + 0x100000, 0x41000}, {BASE + 0x200000, 0x1000}};
    char path[1024];
    Pool pool;
    SnxU64 trap = 0;
    NceSites sites = *found, back;
    NceRange* read_ranges;
    size_t count;
    pool_init(&pool, BASE + 0x100000);
    check(!nce_trap(&pool.pool, 1, &env, NCE_TRAP, &trap) && trap == pool.pool.addr, "the trap");
    check(word(&pool, trap + 12) == (0xD2800010u | NCE_TRAP << 5) &&
              word(&pool, trap + 16) == (0xF2A00010u | (SnxU32)NCE_HOOK << 5) && word(&pool, trap + 40) == 0xD61F0200u,
          "the trap's outcall, which goes on at the runtime's pc");
    check(nce_stub_back(0xD61F0200u, trap + 28) == 0, "a hook's stub has no way back of its own");
    check(nce_stub_back(0x14000000u | ((0x40u >> 2) & 0x3FFFFFFu), 0x1000) == 0x1000 + 12 + 0x40, "a B's way back");
    check(nce_pool_size(found, 0, ~(SnxU64)0) % 0x1000 == 0 && nce_pool_size(found, 0, ~(SnxU64)0) > NCE_HOOK_ROOM,
          "a pool's size, in pages, with the hooks' room");
    /* a copy of the sites moved up by a module's move, then the file */
    sites.site = (NceSite*)snx_alloc(found->count * sizeof *sites.site);
    memcpy(sites.site, found->site, found->count * sizeof *sites.site);
    nce_sites_move(&sites, AT(0), AT(20), 0x10000);
    check(sites.site[0].addr == AT(0) + 0x10000 && site_at(&sites, AT(21)), "the moves of the sites of one range only");
    snprintf(path, sizeof path, "%s/nce_patch_test.nce", dir);
    check(!nce_file_write(path, BASE, ranges, 2, &sites), "the file is written");
    {
        size_t size = 0;
        unsigned char* raw = snx_read_file(path, &size);
        check(raw && size > 40 && raw[7] == 2 && snx_ld_le64(raw + 24) == ranges[0].addr - BASE &&
                  snx_ld_le64(raw + 24 + 2 * 16) == sites.site[0].addr - BASE,
              "version 2, each address from the first module's base");
        free(raw);
    }
    /* read back as Android loads it: the first module moved up to the base its runtime picked (mem.c) */
    check(!nce_file_read(path, BASE + MOVED, &read_ranges, &count, &back), "the file is read");
    check(count == 2 && read_ranges[1].addr == ranges[1].addr + MOVED && read_ranges[0].size == ranges[0].size,
          "its pools, moved with the base");
    check(back.count == sites.count && back.proven == sites.proven && back.unproven == sites.unproven, "its sites");
    for (size_t i = 0; i < back.count && i < sites.count; i++)
        check(back.site[i].addr == sites.site[i].addr + MOVED && back.site[i].word == sites.site[i].word &&
                  back.site[i].kind == sites.site[i].kind && back.site[i].reg == sites.site[i].reg &&
                  back.site[i].imm == sites.site[i].imm && back.site[i].proven == sites.site[i].proven,
              "each site as it was written, moved with the base");
    free(read_ranges);
    nce_sites_free(&back);
    free(sites.site);
    remove(path);
}

int main(int argc, char** argv) {
    NceSites sites;
    scan_test(&sites);
    stub_test(&sites);
    indirect_test(&sites);
    trap_file_test(&sites, argc > 1 ? argv[1] : ".");
    nce_sites_free(&sites);
    divert_test();
    refused_test();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("nce_patch: ok");
    return 0;
}
