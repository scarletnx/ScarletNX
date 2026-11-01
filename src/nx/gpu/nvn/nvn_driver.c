/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The answers of the game's own NVN driver, read from its code. Each game carries its driver, and a game checks what
   nvnDeviceGetInteger says (its API version against the one it was built for, the GPU code versions against its
   shaders), so the runtime answers as that driver would. nvn_driver_info reads the constant a case of the driver's
   switch returns: tools/nvn_census.py's api_version, in C.

   The switch is in nvnDeviceGetInteger or in the first function it calls (SDK 12 and 20 drivers have it in the
   callee): a bounded index (cmp wN, #max; b.hi), a jump table of byte or halfword entries scaled by 4 from an ADR base
   or of signed words relative to the table, and a BR. A case either sets w0 to a constant, or keeps the value w0 was
   given before the BR (the default), or reads the driver's state, which is unknown here */
#include "nx/gpu/nvn/nvn_driver.h"
#include "common/utilities/endian.h"
#include "nx/hle/hook/hook.h"
#include <stdio.h>

typedef struct {
    const u8* mem;
    u64 lo, hi;
} Image;

static int load(const Image* m, u64 a, u32 size, u32* v) {
    if (a < m->lo || a > m->hi || m->hi - a < size) return 0;
    if (size == 4)
        *v = snx_ld_le32(m->mem + a);
    else if (size == 2)
        *v = (u32)m->mem[a] | (u32)m->mem[a + 1] << 8;
    else
        *v = m->mem[a];
    return 1;
}
static i64 sext(u64 v, unsigned bits) { return (i64)(v << (64 - bits)) >> (64 - bits); }

/* a logical immediate's value, 0 for a reserved encoding */
static u64 bitmask_imm(u32 n, u32 immr, u32 imms, unsigned bits) {
    u32 combined = n << 6 | (~imms & 0x3f);
    unsigned len = 0, esize, i;
    u64 elem, out = 0, emask;
    u32 s, r;
    while (len < 7 && combined >> (len + 1)) len++;
    if (!combined || len < 1) return 0;
    esize = 1u << len;
    s = imms & (esize - 1);
    r = immr & (esize - 1);
    if (s == esize - 1) return 0;
    emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    elem = (1ull << (s + 1)) - 1;
    if (r) elem = ((elem >> r) | (elem << (esize - r))) & emask;
    for (i = 0; i < bits; i += esize) out |= elem << i;
    return out;
}
/* whether x sets register rd to a constant (MOVZ, MOVN, ORR of a logical immediate with the zero register): 1 and *v */
static int constant(u32 x, u32* rd, u64* v) {
    unsigned bits = x >> 31 ? 64 : 32;
    u64 mask = bits == 64 ? ~0ull : 0xffffffffull, imm = (x >> 5) & 0xffff;
    unsigned shift = ((x >> 21) & 3) * 16;
    *rd = x & 31;
    if ((x & 0x7f800000) == 0x52800000) { /* movz */
        *v = imm << shift;
        return 1;
    }
    if ((x & 0x7f800000) == 0x12800000) { /* movn */
        *v = ~(imm << shift) & mask;
        return 1;
    }
    if ((x & 0x7f8003e0) == 0x320003e0) { /* orr xd, xzr, #imm */
        *v = bitmask_imm((x >> 22) & 1, (x >> 16) & 0x3f, (x >> 10) & 0x3f, bits);
        return *v != 0;
    }
    return 0;
}
/* whether x may write general register r: a cheap over-approximation by encoding group (nvn_census.py's redefines) */
static int redefines(u32 x, u32 r) {
    u32 group = (x >> 25) & 0xf;
    if (group == 0x8 || group == 0x9 || group == 0x5 || group == 0xd) return (x & 31) == r;
    if ((group & 0x5) == 0x4) {
        int pair = (x & 0x3a000000) == 0x28000000;
        if ((x & 0x3f000000) == 0x08000000) return (x & (1u << 22)) ? (x & 31) == r : ((x >> 16) & 31) == r;
        if (pair || (x & 0x3b200c00) == 0x38000400 || (x & 0x3b200c00) == 0x38000c00)
            if (((x >> 5) & 31) == r && (!pair || (x & (1u << 23)))) return 1;
        if (x & (1u << 26)) return 0;
        if (pair) return (x & (1u << 22)) && ((x & 31) == r || ((x >> 10) & 31) == r);
        if ((x & 0x3b000000) == 0x18000000) return (x & 31) == r;
        return ((x >> 22) & 3) && (x & 31) == r;
    }
    if (group == 0xa || group == 0xb)
        return (((x & 0x7c000000) == 0x14000000 && (x >> 31)) || (x & 0xfffffc1f) == 0xd63f0000) && (r <= 18 || r == 30);
    return 0;
}

/* The switch starting at start: the jump table's address and entry size, the base its byte and halfword entries are
   scaled from, the value w0 holds at the BR, and the largest index the bound lets through. 1 when found */
typedef struct {
    u64 table, base;
    u32 size, bound;
    int has_default, has_bound;
    u64 fallback;
} Switch;
static int find_switch(const Image* m, u64 start, Switch* s) {
    u64 addr[32] = {0};
    u32 known = 0, j; /* bit r: addr[r] holds an address */
    s->size = 0;
    s->has_default = s->has_bound = 0;
    s->base = 0;
    for (j = 0; j < 64; j++) {
        u64 pc = start + 4 * (u64)j, v;
        u32 x, rn, rd;
        if (!load(m, pc, 4, &x)) return 0;
        rn = (x >> 5) & 31;
        if ((x & 0x9f000000) == 0x90000000) { /* adrp */
            addr[x & 31] = (pc & ~0xfffull) + (u64)(sext(((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2), 21) << 12);
            known |= 1u << (x & 31);
        } else if ((x & 0x9f000000) == 0x10000000) { /* adr */
            addr[x & 31] = pc + (u64)sext(((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2), 21);
            known |= 1u << (x & 31);
        } else if ((x & 0xff800000) == 0x91000000 && (known >> rn & 1)) { /* add xd, xn, #imm after an ADRP */
            addr[x & 31] = addr[rn] + ((x >> 10) & 0xfff);
            known |= 1u << (x & 31);
        } else if ((x & 0xffe0ec00) == 0x38606800 && (known >> rn & 1)) { /* ldrb wt, [xn, xm] */
            s->table = addr[rn];
            s->size = 1;
        } else if ((x & 0xffe0ec00) == 0x78606800 && (known >> rn & 1)) { /* ldrh wt, [xn, xm{, lsl #1}] */
            s->table = addr[rn];
            s->size = 2;
        } else if ((x & 0xffe0fc00) == 0xb8a07800 && (known >> rn & 1)) { /* ldrsw xt, [xn, xm, lsl #2] */
            s->table = addr[rn];
            s->size = 4;
        } else if ((x & 0xffe0fc00) == 0x8b000800 && (known >> rn & 1)) { /* add xd, xbase, xm, lsl #2 */
            s->base = addr[rn];
        } else if ((x & 0x7f80001f) == 0x7100001f) { /* cmp wn, #imm (subs wzr): the switch's bound */
            s->bound = (x >> 10) & 0xfff;
            s->has_bound = 1;
        } else if (constant(x, &rd, &v) && rd == 0) {
            s->fallback = v;
            s->has_default = 1;
        } else if ((x & 0xfffffc1f) == 0xd61f0000) /* br */
            return s->size && s->has_bound && (s->size == 4 || s->base);
    }
    return 0;
}
static int case_value(const Image* m, const Switch* s, u32 item, i32* out) {
    u32 entry, k;
    u64 target, v;
    if (item > s->bound || !load(m, s->table + (u64)item * s->size, s->size, &entry)) return 0;
    target = s->size == 4 ? s->table + (u64)sext(entry, 32) : s->base + (u64)entry * 4;
    for (k = 0; k < 8; k++) {
        u32 x, rd;
        if (!load(m, target + 4 * (u64)k, 4, &x)) return 0;
        if (constant(x, &rd, &v) && rd == 0) {
            *out = (i32)v;
            return 1;
        }
        if ((x & 0xfc000000) == 0x14000000 || (x & 0xfffffc1f) == 0xd65f0000) { /* b, ret: the default */
            if (!s->has_default) return 0;
            *out = (i32)s->fallback;
            return 1;
        }
        if (redefines(x, 0)) return 0;
    }
    return 0;
}

/* ---- A32: a 32-bit program's driver (its code in A32 state, as the 32-bit SDK builds it) ----
   The same switch: a bound (cmp rN, #max), and a jump either through a table of words relative to the table, which an
   ADR (add rT, pc, #imm) names (ldr rX, [rI, rT]; add pc, rX, rT), or through a table of absolute addresses right
   after the jump (ldr pc, [pc, rI, lsl #2]). A case sets r0 to a constant, keeps the value r0 got before the jump, or
   reads the driver's state */
static u32 a32_ror_imm(u32 x) {
    u32 imm = x & 0xff, rot = ((x >> 8) & 15) * 2;
    return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
}
/* whether x, unconditional, sets r0 from a constant (mov, mvn, movw) or its top half (movt): 1 and *v updated */
static int a32_constant_r0(u32 x, u32* v) {
    if (x >> 28 != 0xe || ((x >> 12) & 15) != 0) return 0;
    if ((x & 0x0fef0000) == 0x03a00000) { /* mov r0, #imm */
        *v = a32_ror_imm(x);
        return 1;
    }
    if ((x & 0x0fef0000) == 0x03e00000) { /* mvn r0, #imm */
        *v = ~a32_ror_imm(x);
        return 1;
    }
    if ((x & 0x0ff00000) == 0x03000000) { /* movw r0, #imm16 */
        *v = ((x >> 4) & 0xf000) | (x & 0xfff);
        return 1;
    }
    if ((x & 0x0ff00000) == 0x03400000) { /* movt r0, #imm16 */
        *v = (*v & 0xffff) | (((x >> 4) & 0xf000) | (x & 0xfff)) << 16;
        return 1;
    }
    return 0;
}
/* whether x returns: bx lr, or a pop (ldm sp!) that loads pc */
static int a32_returns(u32 x) { return x == 0xe12fff1e || ((x & 0xffff0000) == 0xe8bd0000 && (x & 0x8000)); }
/* whether x leaves r0 alone for sure: a store, a compare, a change of sp, a pop without r0 or pc */
static int a32_keeps_r0(u32 x) {
    u32 op = (x >> 21) & 15;
    if (x >> 28 != 0xe) return 0;
    if ((x & 0x0e000000) == 0x02000000 || (x & 0x0e000010) == 0) /* data processing */
        return (op >= 8 && op <= 11) || ((x >> 12) & 15) != 0;   /* tst, teq, cmp, cmn, or rd isn't r0 */
    if ((x & 0x0c100000) == 0x04000000) return 1;                /* str */
    if ((x & 0x0fff0000) == 0x08bd0000) return !(x & 1);         /* pop without r0 */
    return 0;
}

typedef struct {
    u64 table;    /* the table's address */
    int absolute; /* its words are addresses, else offsets from table */
    u32 bound;    /* the largest index the bound lets through */
    int has_default;
    u32 fallback; /* r0 at the jump */
} Switch32;
static int find_switch32(const Image* m, u64 start, Switch32* s) {
    u64 addr[16] = {0};
    u32 known = 0, j, r0 = 0; /* bit r: addr[r] holds an address */
    int has_bound = 0;
    s->has_default = 0;
    for (j = 0; j < 64; j++) {
        u64 pc = start + 4 * (u64)j;
        u32 x, rn, rm, rd;
        if (!load(m, pc, 4, &x) || a32_returns(x)) return 0; /* not past the function's return */
        rn = (x >> 16) & 15;
        rd = (x >> 12) & 15;
        rm = x & 15;
        if ((x & 0x0fff0000) == 0x028f0000) { /* add rd, pc, #imm (adr) */
            addr[rd] = pc + 8 + a32_ror_imm(x);
            known |= 1u << rd;
        } else if ((x & 0x0ff0f000) == 0x03500000) { /* cmp rn, #imm: the bound */
            s->bound = a32_ror_imm(x);
            has_bound = 1;
        } else if ((x & 0x0f000000) == 0x0b000000) { /* bl: r0 is the callee's */
            s->has_default = 0;
        } else if (a32_constant_r0(x, &r0)) {
            s->fallback = r0;
            s->has_default = 1;
        } else if ((x & 0x0e50f010) == 0x0610f000 && rn == 15 && (x & 0x00800000) && ((x >> 5) & 3) == 0 &&
                   ((x >> 7) & 31) == 2) { /* ldr pc, [pc, rm, lsl #2]: the table follows */
            s->table = pc + 8;
            s->absolute = 1;
            return has_bound;
        } else if ((x & 0x0ff0f010) == 0x0080f000 && (known >> rn & 1) != (known >> rm & 1) && !((x >> 7) & 31)) {
            /* add pc, rn, rm, one of them the table's address */
            s->table = known >> rn & 1 ? addr[rn] : addr[rm];
            s->absolute = 0;
            return has_bound;
        }
    }
    return 0;
}
static int case_value32(const Image* m, const Switch32* s, u32 item, i32* out) {
    u32 entry, k, follows = 0, r0 = s->fallback;
    int r0_known = s->has_default;
    u64 target;
    if (item > s->bound || !load(m, s->table + (u64)item * 4, 4, &entry)) return 0;
    target = s->absolute ? (u64)entry : s->table + (u64)entry;
    for (k = 0; k < 12; k++) {
        u32 x;
        if (!load(m, target, 4, &x)) return 0;
        if (a32_constant_r0(x, &r0)) {
            r0_known = 1;
        } else if (a32_returns(x)) {
            if (!r0_known) return 0;
            *out = (i32)r0;
            return 1;
        } else if ((x & 0xff000000) == 0xea000000 && follows++ < 2) { /* b: the shared return */
            target = target + 8 + (u64)(sext(x & 0xffffff, 24) * 4);
            continue;
        } else if (!a32_keeps_r0(x)) {
            return 0;
        }
        target += 4;
    }
    return 0;
}

int nvn_driver_info32(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    Image m;
    u64 starts[2];
    u32 n = 1, j, i;
    m.mem = mem;
    m.lo = lo;
    m.hi = hi;
    starts[0] = func;
    for (j = 0; j < 16; j++) { /* the first BL */
        u32 x;
        if (!load(&m, func + 4 * (u64)j, 4, &x)) return 0;
        if ((x & 0x0f000000) == 0x0b000000 && x >> 28 == 0xe) {
            starts[n++] = func + 4 * (u64)j + 8 + (u64)(sext(x & 0xffffff, 24) * 4);
            break;
        }
    }
    for (i = 0; i < n; i++) {
        Switch32 s;
        if (find_switch32(&m, starts[i], &s)) return case_value32(&m, &s, item, out);
    }
    return 0;
}

u32 nvn_api_pack(i32 major, i32 minor) {
    if (major <= 0 || major > 0xFFFF || minor < 0 || minor > 0xFFFF) return 0;
    return (u32)major << 16 | (u32)minor;
}

/* the described versions (nvn_driver.h), inclusive ranges in order */
static const struct {
    u32 lo, hi;
} described[] = {{53u << 16 | 5, 53u << 16 | 5},     {53u << 16 | 13, 53u << 16 | 13},
                 {53u << 16 | 105, 53u << 16 | 105}, {53u << 16 | 209, 53u << 16 | 209},
                 {53u << 16 | 311, 53u << 16 | 314}, {55u << 16 | 8, 55u << 16 | 15}};
#define DESCRIBED (sizeof described / sizeof *described)

int nvn_api_described(u32 api) {
    size_t i;
    for (i = 0; i < DESCRIBED; i++)
        if (api >= described[i].lo && api <= described[i].hi) return 1;
    return 0;
}

void nvn_api_described_text(char* out, size_t n) {
    size_t i, at = 0;
    if (!n) return;
    out[0] = 0;
    for (i = 0; i < DESCRIBED && at < n; i++) {
        char lo[32], hi[32];
        int w;
        hook_version_text(lo, sizeof lo, HOOK_NVN, described[i].lo);
        hook_version_text(hi, sizeof hi, HOOK_NVN, described[i].hi);
        w = described[i].lo == described[i].hi ? snprintf(out + at, n - at, "%s%s", i ? ", " : "", lo)
                                                : snprintf(out + at, n - at, "%s%s to %s", i ? ", " : "", lo, hi);
        if (w < 0) return;
        at += (size_t)w;
    }
}

int nvn_driver_info(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    Image m;
    u64 starts[2];
    u32 n = 1, j, i;
    m.mem = mem;
    m.lo = lo;
    m.hi = hi;
    starts[0] = func;
    for (j = 0; j < 16; j++) { /* the first BL */
        u32 x;
        if (!load(&m, func + 4 * (u64)j, 4, &x)) return 0;
        if ((x & 0xfc000000) == 0x94000000) {
            starts[n++] = func + 4 * (u64)j + (u64)(sext(x & 0x3ffffff, 26) * 4);
            break;
        }
    }
    for (i = 0; i < n; i++) {
        Switch s;
        if (find_switch(&m, starts[i], &s)) return case_value(&m, &s, item, out);
    }
    return 0;
}
