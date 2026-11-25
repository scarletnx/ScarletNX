/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What RtlVirtualUnwind makes of a frame of ARM64 code by its .xdata record (Microsoft's ARM64 exception handling), for
   the tests of the function tables the arm64 back end writes: the codes of the prologue's instructions run so far, of an
   epilogue's not run yet, or all of the prologue's in the body. It knows the codes the back end's frames have, and
   nothing else */
#ifndef SNX_TEST_ARM64_UNWINDER_H
#define SNX_TEST_ARM64_UNWINDER_H
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct Arm64State {
    uint64_t pc, sp, x[31];
    uint64_t low, high; /* the stack's bounds: a frame's registers are read from there alone */
} Arm64State;

/* a record's parts: the words of the code it is of, its epilogues' scopes, its codes */
typedef struct {
    uint32_t words;
    size_t epilogues, code_bytes;
    const unsigned char *scopes, *codes;
} Arm64Record;
static inline Arm64Record arm64_record(const unsigned char* p) {
    uint32_t w = snx_ld_le32(p);
    Arm64Record r = {
        .words = w & 0x3FFFF, .epilogues = w >> 22 & 31, .code_bytes = 4 * (size_t)(w >> 27), .scopes = p + 4};
    if (!(w >> 22)) { /* the extended form */
        r.epilogues = snx_ld_le32(p + 4) & 0xFFFF, r.code_bytes = 4 * (size_t)(snx_ld_le32(p + 4) >> 16 & 0xFF);
        r.scopes = p + 8;
    }
    r.codes = r.scopes + 4 * r.epilogues;
    return r;
}
static inline size_t arm64_record_size(const unsigned char* p) {
    Arm64Record r = arm64_record(p);
    return (size_t)(r.codes + r.code_bytes - p);
}

/* a code's bytes, 0 for one not known here */
static inline size_t arm64_code_size(unsigned char c) {
    if (c < 0x20 || (c >= 0x80 && c < 0xC0)) return 1;       /* alloc_s, save_fplr_x */
    if (c >= 0xC0 && c < 0xCC) return 2;                     /* alloc_m, save_regp */
    if (c == 0xE0) return 4;                                 /* alloc_l */
    return c == 0xE1 || c == 0xE3 || c == 0xE4 || c == 0xE5; /* set_fp, nop, end, end_c */
}
/* the instructions of the codes at at, up to an end or an end_c */
static inline uint32_t arm64_instructions(const Arm64Record* r, size_t at) {
    uint32_t n = 0;
    while (at < r->code_bytes && (r->codes[at] & 0xFE) != 0xE4) {
        size_t size = arm64_code_size(r->codes[at]);
        if (!size) return n;
        at += size, ++n;
    }
    return n;
}
/* the pair of registers at at into x: 1, 0 for one off the stack */
static inline int arm64_pair(const Arm64State* s, uint64_t at, uint64_t x[2]) {
    if (at < s->low || at > s->high - 16) return 0;
    memcpy(x, (const void*)(uintptr_t)at, 16);
    return 1;
}

/* s, whose pc is in the code at begin that the record at p is of, made its caller's; NULL, or what can't be */
static inline const char* arm64_unwind(const unsigned char* p, uint64_t begin, Arm64State* s) {
    Arm64Record r = arm64_record(p);
    uint32_t offset = (uint32_t)((s->pc - begin) / 4), skip = 0;
    size_t at = 0, k;
    if (snx_ld_le32(p) >> 18 & 0xF) return "a record of another version, with a handler or of one epilogue";
    if (offset >= r.words) return "a pc past the record's code";
    if (offset < arm64_instructions(&r, 0))
        skip = arm64_instructions(&r, 0) - offset; /* in the prologue: what is left of it was not run */
    else
        for (k = 0; k < r.epilogues; ++k) {
            uint32_t scope = snx_ld_le32(r.scopes + 4 * k), start = scope & 0x3FFFF, code = scope >> 22;
            if (k && start < (snx_ld_le32(r.scopes + 4 * k - 4) & 0x3FFFF)) return "epilogues out of order";
            if (code >= r.code_bytes) return "an epilogue's codes past the record's";
            /* its instructions, and its return or jump */
            if (offset >= start && offset < start + arm64_instructions(&r, code) + 1) {
                at = code, skip = offset - start;
                break;
            }
        }
    while (at < r.code_bytes) {
        const unsigned char* c = r.codes + at;
        size_t size = arm64_code_size(c[0]);
        if (!size) return "a code the back end has none of";
        if (c[0] == 0xE4) {
            s->pc = s->x[30];
            return NULL;
        }
        at += size;
        if (c[0] == 0xE5) continue;
        if (skip) {
            --skip;
            continue;
        }
        if (c[0] < 0x20)
            s->sp += 16ull * c[0];
        else if (c[0] >= 0x80 && c[0] < 0xC0) { /* the pair back, post-indexed */
            if (!arm64_pair(s, s->sp, &s->x[29])) return "a frame's X29 and X30 off the stack";
            s->sp += 8ull * (c[0] & 0x3Fu) + 8;
        } else if (c[0] < 0xC8)
            s->sp += 16ull * ((c[0] & 7u) << 8 | c[1]);
        else if (c[0] < 0xCC) {
            unsigned reg = 19 + ((c[0] & 3u) << 2 | c[1] >> 6);
            if (reg > 28) return "a pair past X28";
            if (!arm64_pair(s, s->sp + 8ull * (c[1] & 0x3F), &s->x[reg])) return "a frame's pair off the stack";
        } else if (c[0] == 0xE0)
            s->sp += 16ull * ((uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3]);
        else if (c[0] == 0xE1)
            s->sp = s->x[29];
    }
    return "codes with no end";
}

#endif
