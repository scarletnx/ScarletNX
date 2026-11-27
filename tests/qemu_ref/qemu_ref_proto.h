/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the QEMU reference's host library (qemu_ref.c) and its AArch64 runner (runner.c) share: the guest layout,
   the batch and result files, and the memory seeds. Both sides include it, the runner freestanding. */
#ifndef SNX_QEMU_REF_PROTO_H
#define SNX_QEMU_REF_PROTO_H

#include <stdint.h>

/* qemu-aarch64's -B: guest address 0 lands here in the host, so a case's window may start at 0 */
#define SNX_QREF_GUEST_BASE   "0x100000000000"
/* the runner's own image (runner.c), far from every window a case maps */
#define SNX_QREF_RUNNER_IMAGE 0x40000000000ull
/* the layout of a single-word case, the same as the native side's: a window, the word at its middle page */
#define SNX_QREF_WINDOW       0x10000000ull
#define SNX_QREF_WINDOW_SIZE  0x220000ull
#define SNX_QREF_WORD_PC      (SNX_QREF_WINDOW + 0x100000ull)
#define SNX_QREF_PAGE         0x1000ull

/* limits of one case */
#define SNX_QREF_MAX_WINDOW   0x400000ull /* bytes of window */
#define SNX_QREF_MAX_CODE     0x10000ull  /* bytes of code region */
#define SNX_QREF_MAX_PATCH    0x10000ull  /* bytes of patches, in all */
#define SNX_QREF_MAX_CHANGED  0x10000ull  /* bytes of changed memory reported; more is truncated */

/* every code slot no word holds: a branch or a fall-through stops on it, and its address is where it went */
#define SNX_QREF_PAD          0xD42B4B40u /* brk #0x5a5a */
#define SNX_QREF_SELF_LOOP    0xD42B4B60u /* brk #0x5a5b, a free run's "b ." */

#define SNX_QREF_MAGIC        0x31464552514e5853ull /* "SNXQREF1" */

/* a case's flags */
#define SNX_QREF_FULL_DIFF    1u /* the window stays writable and is compared whole: no fault between the
                                    instructions of a free run, as exclusive pairs need */

/* how a case's window starts (the seed kinds); patches go over it */
enum {
    SNX_QREF_SEED_ZERO,    /* zeros */
    SNX_QREF_SEED_ADDRESS, /* each byte the low byte of its address */
    SNX_QREF_SEED_NCE,     /* 0x80 | (offset * 0x9E3779B1) >> 25 (no zero byte, so a stored zero shows) */
    SNX_QREF_SEED_RANDOM,  /* splitmix64 of the seed and the offset of each 8 bytes */
    SNX_QREF_SEED_COUNT
};

/* how a case ended */
enum {
    SNX_QREF_NEXT,    /* the last step fell through: pc is the word's + 4 */
    SNX_QREF_BRANCH,  /* the last step went elsewhere: pc is where (a pad or an address it can't fetch) */
    SNX_QREF_UNDEF,   /* SIGILL at the word: pc is the word's */
    SNX_QREF_BRK,     /* BRK at the word: imm is its immediate */
    SNX_QREF_SVC,     /* the last step was an SVC (imm), resumed at pc = the word's + 4 */
    SNX_QREF_FAULT,   /* a data abort or an alignment fault at the word: signo, fault */
    SNX_QREF_TIMEOUT, /* still running after the watchdog's two ticks (a loop) */
    SNX_QREF_ERROR,   /* the case could not run: imm says why */
    SNX_QREF_OUTCOMES
};

/* errors (imm of SNX_QREF_ERROR) */
enum { SNX_QREF_ERR_CASE = 1, SNX_QREF_ERR_MAP, SNX_QREF_ERR_PC };

/* The files are arrays of little-endian u64. The batch: magic, the number of cases, then each case:
     the case words (SNX_QREF_CASE_WORDS), the state (SNX_QREF_STATE_WORDS), the instruction words two to a u64
     (the first in the low half), then each patch: address, length, its bytes padded to 8.
   The result file: magic, the number of cases, then each result: the result words, the state, then each change
   (address, length, its bytes after the run padded to 8), ~0 where the changes end, and 1 when some changes were
   left out (SNX_QREF_MAX_CHANGED), else 0. */
enum {
    SNX_QREF_C_FLAGS,
    SNX_QREF_C_WORDS,
    SNX_QREF_C_PATCHES,
    SNX_QREF_C_WINDOW,
    SNX_QREF_C_WINDOW_SIZE,
    SNX_QREF_C_SEED_KIND,
    SNX_QREF_C_SEED,
    SNX_QREF_C_CODE,
    SNX_QREF_C_CODE_SIZE,
    SNX_QREF_C_PC,
    SNX_QREF_C_STEPS,
    SNX_QREF_CASE_WORDS
};
enum {
    SNX_QREF_R_OUTCOME,
    SNX_QREF_R_SIGNO,
    SNX_QREF_R_SICODE,
    SNX_QREF_R_PC,
    SNX_QREF_R_FAULT,
    SNX_QREF_R_IMM,
    SNX_QREF_R_STEPS,
    SNX_QREF_R_SVCS,
    SNX_QREF_R_PSTATE,
    SNX_QREF_RESULT_WORDS
};
/* the state: X0-X30, SP, NZCV (bits 31-28), FPCR, FPSR, V0-V31 (low then high half) */
enum {
    SNX_QREF_S_X = 0,
    SNX_QREF_S_SP = 31,
    SNX_QREF_S_NZCV,
    SNX_QREF_S_FPCR,
    SNX_QREF_S_FPSR,
    SNX_QREF_S_V,
    SNX_QREF_STATE_WORDS = SNX_QREF_S_V + 64
};

static inline uint64_t snx_qref_splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* the 8 seed bytes at window offset (a multiple of 8) of a window at base, as a little-endian u64 */
static inline uint64_t snx_qref_seed64(unsigned kind, uint64_t seed, uint64_t base, uint64_t offset) {
    uint64_t v = 0;
    unsigned i;
    if (kind == SNX_QREF_SEED_RANDOM) return snx_qref_splitmix(seed ^ snx_qref_splitmix(offset >> 3));
    for (i = 0; i < 8; ++i) {
        uint64_t b = 0, at = offset + i;
        if (kind == SNX_QREF_SEED_ADDRESS)
            b = (base + at) & 0xFF;
        else if (kind == SNX_QREF_SEED_NCE)
            b = 0x80 | (uint32_t)((uint32_t)at * 0x9E3779B1u) >> 25;
        v |= b << (8 * i);
    }
    return v;
}

#endif
