/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's patcher: the words of the game's code that cannot run as they are on an AArch64 host, and the stubs that
   take their place. Eden's patcher (src/core/arm/nce/patcher.cpp) changes the same instructions: SVC, the TLS
   registers, the counter registers and the exclusives. Eden patches each word of .text that has one of their
   encodings. This one patches only the words that the recompiler's analysis (codegen.c) finds as code, and the words
   inside its functions that no code reads as data. Literal pools and tables in .text keep their bytes.

   The patcher does not depend on the host: it makes words, and the tests check them on each host. Only the runtime
   (nce.c) runs them */
#ifndef SNX_NCE_PATCH_H
#define SNX_NCE_PATCH_H
#include "common/cpu/recompiler/codegen.h"

typedef enum {
    NCE_NONE,
    NCE_SVC,       /* SVC #imm: the runtime's SVC, through an outcall */
    NCE_TPIDRRO,   /* MRS Xt, TPIDRRO_EL0: CPU.tpidrro */
    NCE_TPIDR,     /* MRS Xt, TPIDR_EL0: CPU.tpidr */
    NCE_TPIDR_SET, /* MSR TPIDR_EL0, Xt: CPU.tpidr */
    NCE_TICKS,     /* MRS Xt, CNTPCT_EL0 or CNTVCT_EL0: guest_ticks, through an outcall */
    NCE_FREQ,      /* MRS Xt, CNTFRQ_EL0: the console's 19.2 MHz */
    NCE_EXCLUSIVE, /* LDXR, STXR, LDXP, STXP: made ordered in place, with no stub */
    NCE_HOOK       /* the entry of a function that the runtime hooks: an outcall (nce_divert), never in a scan */
} NceKind;

/* the frequency of the console's system counter */
#define NCE_CNTFRQ 19200000u

/* the outcall's x16: the kind in bits 16 and up, the SVC number, the register of NCE_TICKS or the divert's index in
   the low 16 */
#define NCE_OUTCALL(kind, imm) (((SnxU64)(kind) << 16) | ((SnxU64)(imm) & 0xFFFFu))

/* the kind of word, NCE_NONE for one that runs as it is. reg: Rt; imm: the SVC's number */
NceKind nce_kind(SnxU32 word, unsigned* reg, unsigned* imm);

typedef struct {
    SnxU64 addr;
    SnxU32 word;
    unsigned char kind, reg, proven; /* proven: the analysis reaches the word as code */
    unsigned imm;
} NceSite;

typedef struct {
    NceSite* site;
    size_t count, cap;
    /* proven: sites that the analysis reaches; unproven: sites inside a function that no flow reaches and no code
       reads; data: words of a kind that code reads as data (literals, tables); outside: words of a kind outside every
       function. The last two are not in site */
    size_t proven, unproven, data, outside;
} NceSites;

/* the sites in the text of each of codegen's modules, in address order */
void nce_find_sites(const SnxCodegen* codegen, NceSites* out);
void nce_sites_free(NceSites* sites);

/* what the stubs need of the runtime. tls: the distance from the host's TPIDR_EL0 to the thread-local CPU pointer
   (nce_host.c), below 16 MiB and a multiple of 8. With indirect, the word there holds a pointer to the thread's block,
   whose first word is the CPU pointer: tls is then below 32 KiB, an LDR's reach (Android's TLS_SLOT_APP, nce_host.c).
   outcall: the host address of nce_outcall */
typedef struct {
    SnxU64 tls, outcall;
    int indirect;
} NceEnv;

/* where stubs go: size bytes at buf, which the guest sees at addr. used grows, a multiple of 8 */
typedef struct {
    unsigned char* buf;
    SnxU64 addr, size, used;
} NcePool;

/* the bytes of the stub of kind, 0 for NCE_EXCLUSIVE */
SnxU64 nce_stub_size(NceKind kind);
/* the stub of site, in the first pool that has room for it and that it and its return branch reach. *patch: the
   word that goes at site->addr (NCE_EXCLUSIVE: the ordered form, with no stub). 0, or -1 with no pool in reach */
int nce_emit(NcePool* pools, size_t count, const NceEnv* env, const NceSite* site, SnxU32* patch);

/* the stubs of a hooked function at entry, whose first word is first. The outcall stub calls the runtime with
   NCE_OUTCALL(NCE_HOOK, index), and goes on at the pc that the runtime gives back. The original runs first, moved,
   and then goes on at entry + 4: *original is its guest address. *patch: the word that goes at entry. 0, or -1 when
   first cannot move (a PC-relative branch on a condition, or a literal load) or no pool is in reach */
int nce_divert(NcePool* pools, size_t count, const NceEnv* env, SnxU64 entry, SnxU32 first, unsigned index,
               SnxU32* patch, SnxU64* original);

/* the index of NCE_HOOK that no hook has: a trap, where a run of native code that the runtime started returns
   (nce.c) */
#define NCE_TRAP 0xFFFFu

/* a trap: an outcall stub of NCE_OUTCALL(NCE_HOOK, index) that goes on at the pc that the runtime gives back, in the
   first pool with room. A RET or BR reaches it, so it has no reach to keep. *addr: its guest address. 0, or -1 */
int nce_trap(NcePool* pools, size_t count, const NceEnv* env, unsigned index, SnxU64* addr);

/* where the stub whose outcall returns to ret goes back to: the B at ret + 12, word. 0 for the BR X16 of a hook or a
   trap, which goes on at the pc that the runtime gives back */
static inline SnxU64 nce_stub_back(SnxU32 word, SnxU64 ret) {
    SnxU64 imm = word & 0x3FFFFFFu;
    if ((word & 0xFC000000u) != 0x14000000u) return 0;
    if (imm & 0x2000000u) imm |= ~(SnxU64)0 << 26;
    return ret + 12 + (imm << 2);
}

/* the bytes of pool that the sites in [lo, hi) need, in pages of 4 KiB, with NCE_HOOK_ROOM for the hooks */
#define NCE_HOOK_ROOM (256u << 10)
SnxU64 nce_pool_size(const NceSites* sites, SnxU64 lo, SnxU64 hi);
/* the sites in [lo, hi) go up by delta: a module that the NCE recompile moves to make room for a pool */
void nce_sites_move(NceSites* sites, SnxU64 lo, SnxU64 hi, SnxU64 delta);

/* the oldest Android that NCE runs on: from API 29 (Android 10) the app owns bionic's TLS_SLOT_APP, the thread's word
   that the stubs read (nce_host.c); before, it is errno's */
#define NCE_ANDROID_API 29
/* why NCE does not run a 64-bit game on a host that has it: 0 when it does, else -1 with the reason, its values, in
   why. aspace: the NPDM's address space type (1: the 36-bit one, which has to lie below 64 GiB, 3: 39-bit). android:
   the host is Android, at API level api, which moves the guest's addresses above 64 GiB (mem.c) */
int nce_refused(char* why, size_t size, unsigned aspace, int android, int api);

/* the file of an NCE recompile (<game>.nce, beside the module): the guest ranges of the pools, and the sites, each
   address kept as its distance from base, the first module's. The runtime reads them back from the base it loaded
   that module at, which Android moves up (mem.c) */
typedef struct {
    SnxU64 addr, size;
} NceRange;
int nce_file_write(const char* path, SnxU64 base, const NceRange* pools, size_t pool_count, const NceSites* sites);
/* 0, or -1 for a file that is missing or not of this version. *pools is malloc'd */
int nce_file_read(const char* path, SnxU64 base, NceRange** pools, size_t* pool_count, NceSites* sites);

/* the stub layout that the runtime's outcall (nce.c) depends on: the stub lowers the guest's sp by NCE_FRAME and
   keeps x16 and x17 at sp + 0 and 8, x30 at sp + 16. The runtime keeps the stub's return address at sp + 24 */
#define NCE_FRAME 32

#endif
