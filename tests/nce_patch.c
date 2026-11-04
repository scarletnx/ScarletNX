/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's patcher on a synthetic module (src/nx/runtime/nce/nce_patch.c): each kind of word that Eden's patcher
   changes is found in the code, and the words of the same encodings that the code reads as data, or that lie outside
   every function, keep their bytes. Then the stubs: each patched word branches to its stub, the stub does its work in
   the words that the runtime's outcall expects, and comes back to the next word. Words only: any host runs this. The
   net's seccomp filter is words too, run here by a small BPF interpreter, and on Linux by the kernel itself, under the
   threads that take it (nce_net.c) and not under the others */
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
#include <pthread.h>
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

/* x16 = v in four words at a (the stub's MOVZ and three MOVK) */
static int mov16_at(const Pool* pool, SnxU64 a, unsigned d, SnxU64 v) {
    return word(pool, a) == (0xD2800000u | (SnxU32)(v & 0xFFFF) << 5 | d) &&
           word(pool, a + 4) == (0xF2A00000u | (SnxU32)((v >> 16) & 0xFFFF) << 5 | d) &&
           word(pool, a + 8) == (0xF2C00000u | (SnxU32)((v >> 32) & 0xFFFF) << 5 | d) &&
           word(pool, a + 12) == (0xF2E00000u | (SnxU32)((v >> 48) & 0xFFFF) << 5 | d);
}
/* a hook's stubs: the outcall, then the function's own first word, moved. A word that reads the pc keeps its target:
   ADR and ADRP made absolute, B and BL to their targets, a branch on a condition with its taken path a branch of its
   own (through x16 past a B's reach), a literal load from its address made absolute */
static void divert_test(void) {
    static const NceEnv env = {0x10, 0x7F0012345678ull, 0, 0, 0, 0, 0};
    enum { AS_IS, ADRP, B, BL, COND, LIT, PRFM };
    static const struct {
        SnxU32 first;
        int how;
        SnxU64 target; /* from entry */
        SnxU32 moved;  /* COND: the condition two words on; LIT: the load from x(base) */
        unsigned base; /* LIT: the register that holds the address */
        const char* what;
    } cases[] = {
        {0xA9BF7BFDu, AS_IS, 0, 0, 0, "STP X29, X30, [SP, #-16]! moves as it is"},
        {0x90000010u, ADRP, 0, 0, 0, "ADRP X16 becomes its address"},
        {0x14000040u, B, 0x100, 0, 0, "B moves to the stub"},
        {0x94000040u, BL, 0x100, 0, 0, "BL moves to the stub, then on at entry + 4"},
        {0x54000040u, COND, 8, 0x54000040u, 0, "B.EQ keeps its taken path"},
        {0xB4000662u, COND, 0xCC, 0xB4000042u, 0, "CBZ X2 keeps its taken path"},
        {0x37000061u, COND, 12, 0x37000041u, 0, "TBNZ W1, #0 keeps its taken path"},
        {0x58000040u, LIT, 8, 0xF9400000u, 0, "LDR X0 from a literal loads from its address"},
        {0x18000041u, LIT, 8, 0xB9400021u, 1, "LDR W1 from a literal"},
        {0x98000042u, LIT, 8, 0xB9800042u, 2, "LDRSW X2 from a literal"},
        {0x9C000043u, LIT, 8, 0x3DC00203u, 16, "LDR Q3 from a literal, through x16"},
        {0x5800005Fu, LIT, 8, 0xF940021Fu, 16, "a literal load into XZR, through x16 (base 31 is SP)"},
        {0xD8000040u, PRFM, 8, 0, 0, "PRFM of a literal, a hint: nothing to load"},
    };
    SnxU64 entry = BASE + 0x2004;
    SnxU32 patch = 0;
    SnxU64 original = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Pool pool;
        SnxU64 stub, target = entry + cases[i].target;
        pool_init(&pool, BASE + 0x100000);
        stub = pool.pool.addr;
        check(nce_divert(&pool.pool, 1, &env, entry, cases[i].first, 7, &patch, &original) == 0, cases[i].what);
        check(is_b(patch) && b_target(patch, entry) == stub, "the entry branches to the hook's outcall");
        check(word(&pool, stub + 12) == (0xD2800010u | 7u << 5) &&
                  word(&pool, stub + 16) == (0xF2A00010u | (SnxU32)NCE_HOOK << 5),
              "the hook's index in x16");
        check(word(&pool, stub + 40) == 0xD61F0200u, "the hook goes on at the pc that the runtime gives");
        check(original == stub + 56, "the original after the outcall");
        switch (cases[i].how) {
            case AS_IS:
                check(word(&pool, original) == cases[i].first, "the first word as it is");
                check(b_target(word(&pool, original + 4), original + 4) == entry + 4, "then on at entry + 4");
                break;
            case ADRP:
                check(mov16_at(&pool, original, 16, entry & ~(SnxU64)0xFFF), "the page's address in X16");
                check(b_target(word(&pool, original + 16), original + 16) == entry + 4, "then on at entry + 4");
                break;
            case B:
                check(b_target(word(&pool, original), original) == target, "the B's target, and nothing after");
                break;
            case BL:
                check((word(&pool, original) & 0xFC000000u) == 0x94000000u &&
                          b_target(word(&pool, original), original) == target,
                      "the BL's target");
                check(b_target(word(&pool, original + 4), original + 4) == entry + 4, "then on at entry + 4");
                break;
            case COND:
                check(word(&pool, original) == cases[i].moved, "the condition, two words on");
                check(b_target(word(&pool, original + 4), original + 4) == entry + 4, "not taken: on at entry + 4");
                check(b_target(word(&pool, original + 8), original + 8) == target, "taken: on at its target");
                break;
            case LIT:
                check(mov16_at(&pool, original, cases[i].base, target), "the literal's address made absolute");
                check(word(&pool, original + 16) == cases[i].moved, "the load from it");
                check(b_target(word(&pool, original + 20), original + 20) == entry + 4, "then on at entry + 4");
                break;
            default: check(b_target(word(&pool, original), original) == entry + 4, "on at entry + 4"); break;
        }
    }
    {
        /* a CBZ whose target a B from the stub cannot reach (the pool 127.5 MiB up, the target 1 MiB down): the
           taken path branches through x16 */
        Pool pool;
        SnxU32 cbz = 0xB4800000u; /* CBZ X0, entry - 1 MiB: -0x40000 words, 0x40000 in the 19-bit field */
        SnxU64 far = entry - (1ull << 20);
        pool_init(&pool, entry - 4 + (255ull << 19));
        check(nce_divert(&pool.pool, 1, &env, entry, cbz, 7, &patch, &original) == 0 &&
                  b_target(patch, entry) == pool.pool.addr,
              "a CBZ whose target a stub's B cannot reach moves too");
        check(word(&pool, original) == 0xB4000040u && b_target(word(&pool, original + 4), original + 4) == entry + 4 &&
                  mov16_at(&pool, original + 8, 16, far) && word(&pool, original + 24) == 0xD61F0200u,
              "its taken path through x16, BR X16");
    }
    {
        /* a B whose target a stub cannot reach stays: -2, the first word that cannot move; no pool in reach: -1 */
        Pool pool, far;
        pool_init(&pool, entry - 4 + (255ull << 19));
        check(nce_divert(&pool.pool, 1, &env, entry, 0x16000000u, 7, &patch, &original) == -2,
              "a B 128 MiB down from a stub 127.5 MiB up cannot move");
        pool_init(&far, entry + (129ull << 20));
        check(nce_divert(&far.pool, 1, &env, entry, 0xA9BF7BFDu, 7, &patch, &original) == -1,
              "a pool past a B's reach takes no hook");
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

/* ---- the layout of an NCE recompile ---- */

#define MIB (1ull << 20)

/* SVC sites at base + each offset, as the scan finds them in address order */
static void make_sites(NceSites* s, SnxU64 base, const SnxU64* offsets, size_t n) {
    memset(s, 0, sizeof *s);
    s->site = (NceSite*)snx_alloc(n * sizeof *s->site);
    s->count = s->cap = s->proven = n;
    for (size_t i = 0; i < n; i++) {
        memset(&s->site[i], 0, sizeof s->site[i]);
        s->site[i].addr = base + offsets[i];
        s->site[i].word = 0xD4000001u;
        s->site[i].kind = NCE_SVC;
        s->site[i].proven = 1;
    }
}

/* the pool that site i's stub goes to, as the runtime fills them: its index, or -1 */
static int pool_of(const NceRange* ranges, size_t n, const NceSites* s, size_t i) {
    static const NceEnv env = {0x10, 1, 0, 1, 0, 1, 0};
    NcePool* pools = (NcePool*)snx_alloc(n * sizeof *pools);
    SnxU64 trap;
    SnxU32 word;
    int at = -1;
    for (size_t k = 0; k < n; k++) {
        pools[k].buf = (unsigned char*)snx_alloc((size_t)ranges[k].size);
        pools[k].addr = ranges[k].addr;
        pools[k].size = ranges[k].size;
        pools[k].used = 0;
    }
    nce_trap(pools, n, &env, NCE_TRAP, &trap);
    for (size_t k = 0; k <= i; k++)
        if (nce_emit(pools, n, &env, &s->site[k], &word))
            break;
        else if (k == i)
            for (size_t p = 0; p < n; p++)
                if (b_target(word, s->site[k].addr) - pools[p].addr < pools[p].size) at = (int)p;
    for (size_t k = 0; k < n; k++) free(pools[k].buf);
    free(pools);
    return at;
}

static void layout_test(const char* dir) {
    char why[320] = "";
    NceRange* pools = NULL;
    size_t count = 0;
    {
        /* a module of 1 MiB: one pool after it, the module where it was */
        static const SnxU64 at[] = {0x100, 0x80000};
        NceSpan mod = {BASE, MIB};
        NceSites s;
        make_sites(&s, BASE, at, 2);
        check(nce_layout(&mod, 1, BASE + (1ull << 30), &s, &pools, &count, why, sizeof why) == 0, why);
        check(count == 1 && mod.base == BASE && pools[0].addr == BASE + MIB &&
                  pools[0].size == nce_pool_size(&s, BASE, BASE + MIB),
              "a small module keeps its place, its pool after it");
        check(s.site[0].addr == BASE + 0x100 && s.site[1].addr == BASE + 0x80000, "its sites where they were");
        free(pools);
        nce_sites_free(&s);
    }
    {
        /* 200 MiB: its first sites reach no pool after it, so one goes before it */
        static const SnxU64 at[] = {0x100, 64 * MIB, 120 * MIB, 199 * MIB};
        NceSpan mod = {BASE, 200 * MIB};
        NceSites s;
        SnxU64 by;
        make_sites(&s, BASE, at, 4);
        check(nce_layout(&mod, 1, BASE + (1ull << 30), &s, &pools, &count, why, sizeof why) == 0, why);
        check(count == 2 && pools[1].addr == BASE && mod.base == BASE + pools[1].size + 0x1000 &&
                  pools[0].addr == mod.base + 200 * MIB,
              "a module past 128 MiB: a pool before it, which it moves up past, and one after it");
        by = mod.base - BASE;
        check(s.site[0].addr == BASE + 0x100 + by && s.site[3].addr == BASE + 199 * MIB + by,
              "its sites moved with it");
        check(pool_of(pools, count, &s, 0) == 1 && pool_of(pools, count, &s, 1) == 1,
              "its first sites take the one before");
        check(pool_of(pools, count, &s, 2) == 0 && pool_of(pools, count, &s, 3) == 0, "its last ones the one after");
        {
            /* the file keeps the pool below the first module's base, read back from a moved one */
            char path[1024];
            NceRange* back = NULL;
            size_t n = 0;
            NceSites read;
            snprintf(path, sizeof path, "%s/nce_patch_layout.nce", dir);
            check(!nce_file_write(path, mod.base, pools, count, &s) &&
                      !nce_file_read(path, mod.base + MOVED, &back, &n, &read),
                  "the split layout's file, written and read");
            check(n == 2 && back[1].addr == BASE + MOVED && back[0].addr == pools[0].addr + MOVED,
                  "the pool before the module, below its base, moved with it");
            free(back);
            nce_sites_free(&read);
            remove(path);
        }
        free(pools);
        nce_sites_free(&s);
    }
    {
        /* 300 MiB: a word in the middle reaches neither, said with the word, the module and its size, and NCE to
           turn off: the recompile ends there */
        static const SnxU64 at[] = {0x100, 150 * MIB, 299 * MIB};
        NceSpan mod = {BASE, 300 * MIB};
        NceSites s;
        char addr[32], base[32];
        make_sites(&s, BASE, at, 3);
        check(nce_layout(&mod, 1, BASE + (1ull << 30), &s, &pools, &count, why, sizeof why) == 1, "300 MiB is refused");
        snprintf(addr, sizeof addr, "the code at %#llx,", (unsigned long long)(BASE + 150 * MIB));
        snprintf(base, sizeof base, "300 MiB at %#llx,", (unsigned long long)BASE);
        check(strstr(why, addr) && strstr(why, base), "the refusal names the word, the module and its size");
        check(strstr(why, "Set NCE to Off for this game (nce=0) and recompile it") != NULL,
              "and says to turn NCE off for the game");
        check(!pools && mod.base == BASE && s.site[1].addr == BASE + 150 * MIB, "and leaves all as it was");
        nce_sites_free(&s);
    }
    {
        /* a code region too small for the pools: an error that says where they end */
        static const SnxU64 at[] = {0x100};
        NceSpan mod = {BASE, MIB};
        NceSites s;
        make_sites(&s, BASE, at, 1);
        check(nce_layout(&mod, 1, BASE + MIB, &s, &pools, &count, why, sizeof why) == -1 && strstr(why, "code region"),
              "pools past the code region");
        nce_sites_free(&s);
    }
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
/* where the code sits in its page: UBSan's function check (-fsanitize=function) reads the 8 bytes before an indirect
   call's target, which are then the page's own and not the page before it, mapped or not */
#define NET_CODE_AT 64
static uintptr_t g_net_page;
static long (*g_net_fn)(void);
static long g_trapped; /* the thread whose call from the watched page trapped there, by its id */
static void net_sigsys(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    (void)ctx;
    if (si->si_code != 1 || (uintptr_t)si->si_call_addr - g_net_page >= 4096) _exit(43);
    __atomic_store_n(&g_trapped, syscall(SYS_gettid), __ATOMIC_RELEASE); /* the call is not made: it goes on past */
}
/* a thread of the child: what nce_net_install said, its filters and no_new_privs after, and its call from the page */
typedef struct {
    SnxU64 lo;
    int base, took, filters, nnp, trapped;
    long got;
} NetThread;
static void net_call(NetThread* t) {
    long tid = syscall(SYS_gettid);
    __atomic_store_n(&g_trapped, 0, __ATOMIC_RELEASE);
    t->got = g_net_fn();
    t->trapped = __atomic_load_n(&g_trapped, __ATOMIC_ACQUIRE) == tid;
}
static void net_take(NetThread* t) {
    char why[160];
    t->took = nce_net_install(t->lo, t->lo + 4096, NET_ARCH, t->base, why, sizeof why);
    if (t->took < 0) fprintf(stderr, "nce_patch: the net: %s\n", why);
    t->filters = nce_net_filters();
    t->nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
}
static void* net_started(void* arg) { /* a thread that a guest thread starts: it takes the net again */
    net_take((NetThread*)arg);
    net_call((NetThread*)arg);
    return NULL;
}
static void* net_guest(void* arg) { /* t[0], a guest thread, then t[1], the thread it starts */
    NetThread* t = (NetThread*)arg;
    pthread_t th;
    net_take(t);
    net_call(t);
    t[1].lo = t->lo;
    t[1].base = t->base;
    if (t->took >= 0 && !pthread_create(&th, NULL, net_started, &t[1])) pthread_join(th, NULL);
    return NULL;
}
static void* net_runtime(void* arg) { /* a runtime's thread, started after a guest thread took the net */
    net_call((NetThread*)arg);
    return NULL;
}
/* In a child: a guest thread takes the net over the page (inside) or the page after it, calls from the page and starts
   a thread that takes it too; then the runtime's thread (main) and one it starts call from the page. What each saw goes
   in out, which the parent reads; the child ends 77 where the kernel has no seccomp filters */
typedef struct {
    NetThread guest[2], main, late;
    int base, main_filters, main_nnp;
    long pid;
} NetRun;
static void net_child(int inside, NetRun* out) {
    void* page = mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa;
    pthread_t th;
    if (page == MAP_FAILED || prctl(PR_GET_SECCOMP, 0, 0, 0, 0) < 0) _exit(77);
    {
        char* code = (char*)page + NET_CODE_AT;
        memcpy(code, net_code, sizeof net_code);
        __builtin___clear_cache(code, code + sizeof net_code);
        /* ISO C has no conversion from an object pointer to a function pointer */
        memcpy(&g_net_fn, &code, sizeof g_net_fn);
    }
    g_net_page = (uintptr_t)page;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = net_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    out->pid = getpid();
    out->base = nce_net_filters();
    out->guest[0].lo = (SnxU64)g_net_page + (inside ? 0 : 4096);
    out->guest[0].base = out->base;
    if (pthread_create(&th, NULL, net_guest, out->guest) || pthread_join(th, NULL)) _exit(78);
    out->main_filters = nce_net_filters();
    out->main_nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    net_call(&out->main);
    if (pthread_create(&th, NULL, net_runtime, &out->late) || pthread_join(th, NULL)) _exit(78);
    _exit(0);
}
static void net_live_test(void) {
    NetRun* run = (NetRun*)mmap(NULL, 2 * sizeof *run, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int status[2] = {0, 0};
    if (run == MAP_FAILED) {
        check(0, "the net's children share their results");
        return;
    }
    memset(run, 0, 2 * sizeof *run);
    for (int inside = 0; inside < 2; inside++) {
        pid_t pid;
        fflush(NULL);
        pid = fork();
        if (!pid) net_child(inside, &run[inside]);
        check(pid > 0 && waitpid(pid, &status[inside], 0) == pid && WIFEXITED(status[inside]), "the net's child ends");
    }
    if (WEXITSTATUS(status[0]) == 77 || WEXITSTATUS(status[1]) == 77) {
        puts("nce_patch: the kernel took no seccomp filter here: the net's run by the kernel skipped");
        return;
    }
    check(!WEXITSTATUS(status[0]) && !WEXITSTATUS(status[1]), "the net's child ran its threads");
    for (int inside = 0; inside < 2; inside++) {
        const NetRun* r = &run[inside];
        const NetThread *g = &r->guest[0], *s = &r->guest[1];
        check(!g->took && g->nnp == 1, "a guest thread takes the net, with no_new_privs set on it");
        if (inside) {
            check(g->trapped, "the kernel traps the guest thread's system call from the watched page, at it");
            check(s->took == 1 && s->filters == g->filters && s->trapped,
                  "a thread that a guest thread starts has the net already, and no second one");
        } else
            check(!g->trapped && g->got == r->pid && !s->trapped && s->got == r->pid,
                  "and lets through the same call from a page outside the range, and libc's");
        check(r->main_nnp == 0 && r->main_filters == r->base,
              "the runtime's thread keeps its own filters and no_new_privs");
        check(!r->main.trapped && r->main.got == r->pid && !r->late.trapped && r->late.got == r->pid,
              "the runtime's threads, the one before and one started after, make the call from the page");
    }
    munmap(run, 2 * sizeof *run);
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
    layout_test(argc > 1 ? argv[1] : ".");
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
