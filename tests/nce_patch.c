/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's patcher on a synthetic module (src/nx/runtime/nce/nce_patch.c): each kind of word that Eden's patcher
   changes is found in the code, and the words of the same encodings that the code reads as data, or that lie outside
   every function, keep their bytes. Then the stubs: each patched word branches to its stub, the stub does its work in
   the words that the runtime's outcall expects, and comes back to the next word. Words only: any host runs this. The
   net's seccomp filter is words too, run here by a small BPF interpreter, and on Linux by the kernel itself */
#if defined(__linux__)
#define _GNU_SOURCE 1 /* fork, syscall and the SIGSYS fields of siginfo_t, for the net's run by the kernel */
#endif
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
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define NET_LIVE 1
#endif

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
    check(sites->outside_kind[NCE_SVC] == 1, "the one outside is an SVC, counted by its kind");
    check(!strcmp(nce_kind_name(NCE_SVC), "SVC") && !strcmp(nce_kind_name(NCE_TPIDRRO), "MRS TPIDRRO_EL0"),
          "the kinds' names");
}

/* the counter's stub of site at stub, its scratch registers a and b, its block's word reached by the three words of
   tls (direct or indirect, nce_patch.c's load_tls) */
static void ticks_words(const Pool* pool, const NceEnv* env, const NceSite* s, SnxU64 stub, const SnxU32 tls[3]) {
    SnxU32 t = s->reg, a = t < 2 ? 2 : 0, b = a + 1;
    SnxU64 lit = stub + 144;
    check(word(pool, stub) == (0xA9BF0000u | b << 10 | 31u << 5 | a), "two scratch registers below the guest's sp");
    check(word(pool, stub + 4) == tls[0] && word(pool, stub + 8) == tls[1] && word(pool, stub + 12) == tls[2],
          "the thread's count of reads, from its block");
    check(word(pool, stub + 16) == (0xF9400000u | a << 5 | b) && word(pool, stub + 20) == (0x91000400u | b << 5 | b) &&
              word(pool, stub + 24) == (0xF9000000u | a << 5 | b),
          "the count goes up by one");
    check(word(pool, stub + 28) == (0x58000000u | (SnxU32)((lit - (stub + 28)) >> 2) << 5 | a) &&
              word(pool, stub + 32) == (0xC8DFFC00u | a << 5 | b),
          "frozen, loaded with acquire from the clock's first word");
    check(word(pool, stub + 36) == (0xB5000000u | (SnxU32)((stub + 132 - (stub + 36)) >> 2) << 5 | b),
          "a pause's value goes to the frozen path");
    check(word(pool, stub + 40) == 0xD5033FDFu && word(pool, stub + 44) == (0xD53BE040u | t),
          "the host's counter, after an ISB");
    check(word(pool, stub + 52) == (0x9BC07C00u | b << 16 | t << 5 | b) &&
              word(pool, stub + 60) == (0x9B000000u | a << 16 | b << 10 | t << 5 | t),
          "nanoseconds: UMULH by the fraction, MADD by the whole part");
    check(word(pool, stub + 68) == (0xCB000000u | a << 16 | t << 5 | t) &&
              word(pool, stub + 76) == (0x91002000u | a << 5 | a) &&
              word(pool, stub + 80) == (0xC8DFFC00u | a << 5 | b) &&
              word(pool, stub + 84) == (0xCB000000u | b << 16 | t << 5 | t),
          "less origin, then start, loaded with acquire after frozen");
    check(word(pool, stub + 88) == (0xD2800000u | 10000u << 5 | a) &&
              word(pool, stub + 92) == (0x9AC00800u | a << 16 | t << 5 | b) &&
              word(pool, stub + 96) == (0x9B008000u | a << 16 | t << 10 | b << 5 | t),
          "ns / 10000 and ns % 10000");
    check(word(pool, stub + 100) == (0xD2800000u | 192u << 5 | a) &&
              word(pool, stub + 104) == (0x9B007C00u | a << 16 | t << 5 | t) &&
              word(pool, stub + 108) == (0x9B007C00u | a << 16 | b << 5 | b) &&
              word(pool, stub + 116) == (0x9AC00800u | a << 16 | t << 5 | t) &&
              word(pool, stub + 120) == (0x8B000000u | b << 16 | t << 5 | t),
          "192 ticks every 10 us, as guest_ticks counts them");
    check(word(pool, stub + 124) == (0xA8C10000u | b << 10 | 31u << 5 | a), "the scratch registers back");
    check(b_target(word(pool, stub + 128), stub + 128) == s->addr + 4, "back at the next word");
    check(word(pool, stub + 132) == (0xD1000400u | b << 5 | t) &&
              b_target(word(pool, stub + 136), stub + 136) == stub + 88,
          "frozen less 1, then the ticks");
    check(snx_ld_le64(pool->buf + (size_t)(lit - pool->pool.addr)) == env->clock &&
              snx_ld_le64(pool->buf + (size_t)(lit + 8 - pool->pool.addr)) == env->ns_lo &&
              snx_ld_le64(pool->buf + (size_t)(lit + 16 - pool->pool.addr)) == env->ns_hi &&
              snx_ld_le64(pool->buf + (size_t)(lit + 24 - pool->pool.addr)) == env->ns_origin,
          "the clock's address and its reading in the counter, 8-aligned after the code");
    check(word(pool, stub + 48) == (0x58000000u | (SnxU32)((lit + 8 - (stub + 48)) >> 2) << 5 | b) &&
              word(pool, stub + 56) == (0x58000000u | (SnxU32)((lit + 16 - (stub + 56)) >> 2) << 5 | a) &&
              word(pool, stub + 64) == (0x58000000u | (SnxU32)((lit + 24 - (stub + 64)) >> 2) << 5 | a) &&
              word(pool, stub + 72) == (0x58000000u | (SnxU32)((lit - (stub + 72)) >> 2) << 5 | a),
          "each literal loaded where it is used");
}

static void stub_test(const NceSites* sites) {
    /* tls past a page, to take the ADD */
    static const NceEnv env = {0x1010, 0x7F0012345678ull, 0, 0x7F00AABB0000ull, 0x1555555555555555ull, 52, 0x123456789};
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
        case NCE_TICKS: {
            SnxU32 a = s->reg < 2 ? 2 : 0;
            const SnxU32 tls[3] = {0xD53BD040u | a, 0x91400000u | 1u << 10 | a << 5 | a,
                                   0xF9400000u | ((0x10u + NCE_TLS_READS) / 8) << 10 | a << 5 | a};
            ticks_words(&pool, &env, s, stub, tls);
            check(pool.pool.used % 8 == 0, "the next stub on 8");
            break;
        }
        case NCE_SVC: {
            SnxU64 code = NCE_OUTCALL(NCE_SVC, s->imm);
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
        NceEnv no_clock = env;
        Pool spare;
        check(nce_emit(&only, 1, &env, &s, &patch) == -1, "no stub where no pool is in reach");
        pool_init(&spare, BASE + 0x100000);
        no_clock.clock = 0;
        check(nce_emit(&spare.pool, 1, &no_clock, site_at(sites, AT(4)), &patch) == -1 && !spare.pool.used,
              "no counter's stub without the clock's address");
    }
}

/* the indirect form (Android's TLS_SLOT_APP): the word at tls holds a pointer to the thread's block, whose first word
   is the CPU pointer. The same three words, so the stubs keep their sizes; tls within an LDR's 32 KiB */
static void indirect_test(const NceSites* sites) {
    static const NceEnv env = {0x10, 0x7F0012345678ull, 1, 0x7F00AABB0000ull, 0x1555555555555555ull, 52, 0x123456789};
    NceEnv far = env;
    Pool pool;
    SnxU32 patch;
    unsigned tested = 0;
    pool_init(&pool, BASE + 0x100000);
    for (size_t i = 0; i < sites->count; i++) {
        const NceSite* s = &sites->site[i];
        SnxU64 stub = pool.pool.addr + pool.pool.used, used = pool.pool.used;
        SnxU32 t = s->reg, cpu;
        if (s->kind == NCE_TICKS) {
            SnxU32 a = t < 2 ? 2 : 0;
            const SnxU32 tls[3] = {0xD53BD040u | a, 0xF9400000u | (0x10u / 8) << 10 | a << 5 | a,
                                   0xF9400000u | (NCE_TLS_READS / 8) << 10 | a << 5 | a};
            check(!nce_emit(&pool.pool, 1, &env, s, &patch), "an indirect counter's stub");
            check(pool.pool.used - used == nce_stub_size(NCE_TICKS), "of the direct one's size");
            ticks_words(&pool, &env, s, stub, tls);
            continue;
        }
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
    static const NceEnv env = {0x10, 0x7F0012345678ull, 0, 0, 0, 0, 0};
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
    static const NceEnv env = {0x10, 0x7F0012345678ull, 0, 0, 0, 0, 0};
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
        check(raw && size > 40 && raw[7] == 3 && snx_ld_le64(raw + 24) == ranges[0].addr - BASE &&
                  snx_ld_le64(raw + 24 + 2 * 16) == sites.site[0].addr - BASE,
              "version 3, each address from the first module's base");
        if (raw) { /* a file of version 2, whose pools are sized for the counter's outcall stub, is refused */
            NceRange* old_ranges = NULL;
            size_t old_count = 0;
            raw[7] = 2;
            check(!snx_write_file(path, raw, size), "a version 2 file is written");
            check(nce_file_read(path, BASE, &old_ranges, &old_count, &back) == -1 && !old_ranges,
                  "a version 2 file is refused");
            check(!nce_file_write(path, BASE, ranges, 2, &sites), "the file is written again");
        }
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

/* ---- the net ---- */

#define ACT_TRAP  0x00030000u /* SECCOMP_RET_TRAP */
#define ACT_ALLOW 0x7FFF0000u /* SECCOMP_RET_ALLOW */
#define ARCH_A64  0xC00000B7u /* AUDIT_ARCH_AARCH64 */

/* classic BPF as the kernel runs a seccomp filter, for the words that nce_net_filter makes: the action for a system
   call of arch made from ip, the seccomp_data's words little-endian */
static SnxU32 run_bpf(const NceBpf* prog, size_t n, SnxU32 arch, SnxU64 ip) {
    unsigned char data[64];
    SnxU32 a = 0;
    memset(data, 0, sizeof data);
    snx_st_le32(data + 4, arch);
    snx_st_le64(data + 8, ip);
    for (size_t pc = 0; pc < n; pc++) {
        const NceBpf* b = &prog[pc];
        switch (b->code) {
            case 0x20: a = b->k + 4 <= sizeof data ? snx_ld_le32(data + b->k) : 0; break; /* LD W ABS */
            case 0x15: pc += a == b->k ? b->jt : b->jf; break;                            /* JEQ K */
            case 0x25: pc += a > b->k ? b->jt : b->jf; break;                             /* JGT K */
            case 0x35: pc += a >= b->k ? b->jt : b->jf; break;                            /* JGE K */
            case 0x06: return b->k;                                                       /* RET K */
            default: return 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu; /* past the end, which the kernel refuses */
}

/* the filter's range, at the words' edges: [lo, hi) traps, the rest of the addresses and another arch go through */
static void net_words_test(void) {
    static const struct {
        SnxU64 lo, hi;
    } ranges[] = {
        {0x0000000FFFFFF000ull, 0x0000001000003000ull}, /* across a 4 GiB line: the high words differ */
        {0x0000000008000000ull, 0x0000002000000000ull}, /* the console's code region to 128 GiB */
        {0x0000001008000000ull, 0x0000001008200000ull}, /* one high word, as Android's moved layout may have */
    };
    for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++) {
        NceBpf prog[NCE_NET_WORDS];
        SnxU64 lo = ranges[i].lo, hi = ranges[i].hi;
        size_t n = nce_net_filter(lo, hi, ARCH_A64, prog);
        char what[160];
        const struct {
            SnxU64 ip;
            SnxU32 act;
        } calls[] = {{lo - 4, ACT_ALLOW},
                     {lo, ACT_TRAP},
                     {(lo | 0xFFFFFFFFull) < hi ? (lo | 0xFFFFFFFFull) - 3 : lo + 4, ACT_TRAP},
                     {(lo | 0xFFFFFFFFull) + 1 < hi ? (lo | 0xFFFFFFFFull) + 1 : hi - 8, ACT_TRAP},
                     {hi - 4, ACT_TRAP},
                     {hi, ACT_ALLOW},
                     {hi + 0x100000000ull, ACT_ALLOW},
                     {0, ACT_ALLOW},
                     {~(SnxU64)0, ACT_ALLOW}};
        check(n == NCE_NET_WORDS, "the net's words");
        for (size_t k = 0; k < sizeof calls / sizeof *calls; k++) {
            snprintf(what, sizeof what, "a call from %#llx with [%#llx, %#llx) watched: %s",
                     (unsigned long long)calls[k].ip, (unsigned long long)lo, (unsigned long long)hi,
                     calls[k].act == ACT_TRAP ? "trapped" : "let through");
            check(run_bpf(prog, n, ARCH_A64, calls[k].ip) == calls[k].act, what);
        }
        check(run_bpf(prog, n, 0xC000003Eu, lo) == ACT_ALLOW, "another architecture's call goes through");
    }
}

#if NET_LIVE
/* the host's code of a system call (getpid) and a return, in a page of its own */
#if defined(__x86_64__)
#define NET_ARCH AUDIT_ARCH_X86_64
static const unsigned char net_code[] = {0xB8, 0x27, 0, 0, 0, 0x0F, 0x05, 0xC3}; /* MOV EAX, 39; SYSCALL; RET */
#else
#define NET_ARCH AUDIT_ARCH_AARCH64
static const unsigned char net_code[] = {0x88, 0x15, 0x80, 0xD2,  /* MOV X8, #172 */
                                         0x01, 0x00, 0x00, 0xD4,  /* SVC #0 */
                                         0xC0, 0x03, 0x5F, 0xD6}; /* RET */
#endif
static uintptr_t g_net_page;
static void net_sigsys(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    (void)ctx;
    _exit(si->si_code == 1 && (uintptr_t)si->si_call_addr - g_net_page < 4096 ? 42 : 43);
}
/* in a child: the filter over the page (inside) or the page after it, then a call from libc and one from the page.
   The child's end says what the kernel did: 42 the page's call trapped, 0 both went through, 77 no seccomp here */
static void net_child(int inside) {
    void* page = mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long (*fn)(void);
    NceBpf prog[NCE_NET_WORDS];
    struct sock_fprog fp;
    struct sigaction sa;
    SnxU64 lo;
    if (page == MAP_FAILED) _exit(77);
    memcpy(page, net_code, sizeof net_code);
    __builtin___clear_cache((char*)page, (char*)page + sizeof net_code);
    memcpy(&fn, &page, sizeof fn); /* ISO C has no conversion from an object pointer to a function pointer */
    g_net_page = (uintptr_t)page;
    lo = (SnxU64)g_net_page + (inside ? 0 : 4096);
    fp.len = (unsigned short)nce_net_filter(lo, lo + 4096, NET_ARCH, prog);
    fp.filter = (struct sock_filter*)(void*)prog;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = net_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp)) {
        perror("nce_patch: the net's seccomp filter");
        _exit(77);
    }
    if (getppid() <= 0) _exit(44); /* libc's call, outside the range */
    if (fn() != (long)getpid()) _exit(45);
    _exit(0);
}
static void net_live_test(void) {
    int status[2] = {0, 0};
    for (int inside = 0; inside < 2; inside++) {
        pid_t pid;
        fflush(NULL);
        pid = fork();
        if (!pid) net_child(inside);
        check(pid > 0 && waitpid(pid, &status[inside], 0) == pid && WIFEXITED(status[inside]), "the net's child ends");
    }
    if (WEXITSTATUS(status[0]) == 77 || WEXITSTATUS(status[1]) == 77) {
        puts("nce_patch: the kernel took no seccomp filter here: the net's run by the kernel skipped");
        return;
    }
    check(WEXITSTATUS(status[1]) == 42, "the kernel traps a system call made from the watched page, at it");
    check(WEXITSTATUS(status[0]) == 0, "and lets through the same call from a page outside the range, and libc's");
}
#endif

int main(int argc, char** argv) {
    NceSites sites;
    scan_test(&sites);
    stub_test(&sites);
    indirect_test(&sites);
    trap_file_test(&sites, argc > 1 ? argv[1] : ".");
    nce_sites_free(&sites);
    divert_test();
    refused_test();
    net_words_test();
#if NET_LIVE
    net_live_test();
#endif
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("nce_patch: ok");
    return 0;
}
