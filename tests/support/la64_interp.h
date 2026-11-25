/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An interpreter of the LoongArch64 instructions the loongarch64 back end writes (tools/sailgen/td/la64.names), for
   the back ends' tests where Unicorn has no LoongArch (host_unicorn.c's HOSTUC_LA64). The decode table is the same
   LLVM records' as the encoders (la64_dec.inc); the semantics are the LoongArch Reference Manual's, volume 1 (v1.10),
   written here. It runs in the test's own process on its own memory: every access is checked against the regions
   the test gave it, and a word outside the list stops it */
#ifndef SNX_TEST_LA64_INTERP_H
#define SNX_TEST_LA64_INTERP_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t lo, hi; /* [lo, hi) */
} La64Region;

typedef struct La64 La64;
/* a trap: pc reached a word the caller handles (its own returns), the interpreter's state to read and write; 1 to
   go on (with pc set), 0 to stop, -1 an error */
typedef int (*La64Trap)(La64* c, void* user);

struct La64 {
    uint64_t r[32], pc;
    uint64_t ll_addr;
    int ll_on;
    const La64Region* regions;
    size_t nregions;
    uint64_t trap_lo, trap_hi; /* [trap_lo, trap_hi): pc there calls trap */
    La64Trap trap;
    void* user;
    char why[160];
};

/* the decoder's fields checked against the encoders (0, -1 with why in c) */
int la64_selftest(La64* c);
/* run from c->pc for at most limit instructions: 0 where a trap stopped it, -1 with why in c */
int la64_run(La64* c, uint64_t limit);

#endif
