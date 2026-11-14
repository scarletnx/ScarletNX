/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CPU's run of the instructions that compute an Ounce program's texture handles from memory (sm86.h, sm86_eval):
   the integer ALU, the constant buffer loads and the global loads of a handle's address, each as sm86_translate.c
   translates it, on the values a draw binds. The translation keeps those instructions (ShaderInfo.memh) when every
   value they read is a constant buffer's, an immediate or another of them: the same for every invocation of a draw,
   which the renderer can then bind a slot for (vk/render.c's tex_handle). */
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
#include <string.h>

typedef struct {
    u32 r[256], ur[64];
    u8 p[8], up[8];
    const Sm86EvalIo* io;
} Ev;

int sm86_eval_takes(const Sm86Insn* in) {
    switch (in->op) {
        case SM86_MOV:
        case SM86_UMOV:
        case SM86_R2UR:
        case SM86_IADD3:
        case SM86_UIADD3:
        case SM86_IMAD:
        case SM86_UIMAD:
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE:
        case SM86_LEA:
        case SM86_ULEA:
        case SM86_SHF:
        case SM86_USHF:
        case SM86_SEL:
        case SM86_USEL:
        case SM86_ISETP:
        case SM86_UISETP:
        case SM86_PLOP3:
        case SM86_UPLOP3:
        case SM86_IMNMX:
        case SM86_UIMNMX: return 1;
        case SM86_LOP3:
        case SM86_ULOP3: return !sm86_field(in, 80, 1) && sm86_field(in, 87, 4) == 0xf;
        case SM86_PRMT:
        case SM86_UPRMT: return !sm86_field(in, 72, 3);
        case SM86_ULDC: return !sm86_field(in, 91, 1) && sm86_field(in, 73, 3) <= 5;
        case SM86_ULDC_IDX: return sm86_field(in, 73, 3) <= 5;
        case SM86_LDC: return !sm86_field(in, 78, 2) && sm86_field(in, 73, 3) <= 5;
        case SM86_LDG:
        case SM86_LDG_NOU: return sm86_field(in, 73, 3) <= 6;
        case SM86_CS2R: return sm86_field(in, 72, 8) == 0xff;
        default: return 0;
    }
}

static u32 ev_bits(Ev* e, const Sm86Src* s) {
    switch (s->kind) {
        case SM86_SRC_REG: return s->reg < SM86_RZ ? e->r[s->reg] : 0;
        case SM86_SRC_UREG: return s->reg < SM86_URZ ? e->ur[s->reg] : 0;
        case SM86_SRC_IMM: return s->value;
        case SM86_SRC_CBUF: return s->bindless ? 0 : e->io->cbuf(e->io->user, s->bank, s->value);
        default: return 0;
    }
}
/* an integer source with its negation, as src_int */
static u32 ev_int(Ev* e, const Sm86Src* s) {
    u32 v = ev_bits(e, s);
    return s->neg && s->kind != SM86_SRC_IMM ? 0u - v : v;
}
/* a term of a 64-bit sum, as term64: ~x + 1 in 33 bits for a negated one, ~x alone with the carries in (.X) */
static u64 ev_term64(Ev* e, const Sm86Src* s, int x) {
    u32 v = ev_bits(e, s);
    if (!s->neg || s->kind == SM86_SRC_IMM) return v;
    return (u64)(u32)~v + (x ? 0 : 1);
}
static int ev_pred(Ev* e, u32 p, u32 negate, int uniform) {
    int v = p >= SM86_PT ? 1 : (uniform ? e->up : e->p)[p];
    return negate ? !v : v;
}
static void ev_pwr(Ev* e, u32 p, int v, int uniform) {
    if (p < SM86_PT) (uniform ? e->up : e->p)[p] = (u8)(v != 0);
}
static void ev_wr(Ev* e, u32 r, u32 v) {
    if (r < SM86_RZ) e->r[r] = v;
}
static void ev_uwr(Ev* e, u32 r, u32 v) {
    if (r < SM86_URZ) e->ur[r] = v;
}
static void ev_dwr(Ev* e, const Sm86Insn* in, u32 v) {
    if (sm86_ops[in->op].kind == SM86_K_UALU)
        ev_uwr(e, in->dst, v);
    else
        ev_wr(e, in->dst, v);
}
static u32 ev_lut(u32 lut, u32 x, u32 y, u32 z) {
    u32 v = 0;
    for (u32 i = 0; i < 8; i++)
        if (lut >> i & 1) v |= (i & 4 ? x : ~x) & (i & 2 ? y : ~y) & (i & 1 ? z : ~z);
    return v;
}
/* NAK's IntCmpOp (bits 76-78), as int_compare */
static int ev_compare(u32 cmp, int is_signed, u32 x, u32 y) {
    i32 a = (i32)x, b = (i32)y;
    switch (cmp) {
        case 1: return is_signed ? a < b : x < y;
        case 2: return x == y;
        case 3: return is_signed ? a <= b : x <= y;
        case 4: return is_signed ? a > b : x > y;
        case 5: return x != y;
        case 6: return is_signed ? a >= b : x >= y;
        case 7: return 1;
        default: return 0;
    }
}
static int ev_combine(u32 set_op, int x, int acc) { return set_op == 0 ? x && acc : set_op == 1 ? x || acc : x != acc; }
/* a byte or a halfword (NAK's MemType 0-3) of the word that holds byte at, as cbuf_subword and mem_load take it */
static u32 ev_subword(u32 word, u32 at, u32 type) {
    u32 shift = (at & (type < 2 ? 3u : 2u)) * 8, bits = type < 2 ? 8 : 16, v = (word >> shift) & ((1u << bits) - 1);
    return type & 1 && v >> (bits - 1) ? v | ~((1u << bits) - 1) : v;
}
/* a constant buffer word at a byte offset plus an index, its subword for the small types: LDC, ULDC, ULDC.IDX */
static u32 ev_cbuf(Ev* e, u32 bank, u32 off, u32 index, u32 type, u32 k) {
    u32 at = off + index + 4 * k, word = e->io->cbuf(e->io->user, bank, at & ~3u);
    return type < 4 ? ev_subword(word, at, type) : word;
}
/* one instruction: 0 when it is not one sm86_eval_takes, or a load reads memory that isn't there */
static int ev_step(Ev* e, const Sm86Insn* in) {
    int op = in->op, uniform = sm86_ops[op].kind == SM86_K_UALU;
    switch (op) {
        case SM86_MOV: ev_wr(e, in->dst, ev_bits(e, &in->b)); return 1;
        case SM86_UMOV: ev_uwr(e, in->dst, ev_bits(e, &in->b)); return 1;
        case SM86_R2UR: {
            u32 r = sm86_field(in, 24, 8);
            ev_uwr(e, sm86_field(in, 16, 6), r < SM86_RZ ? e->r[r] : 0);
            return 1;
        }
        case SM86_CS2R:
            ev_wr(e, in->dst, 0);
            if (sm86_field(in, 80, 1) && in->dst < SM86_RZ) ev_wr(e, in->dst + 1, 0);
            return 1;
        case SM86_IADD3:
        case SM86_UIADD3: {
            u32 p = sm86_field(in, 81, 3), q = sm86_field(in, 84, 3);
            int x = (int)sm86_field(in, 74, 1);
            u64 sum = ev_term64(e, &in->a, x) + ev_term64(e, &in->b, x) + ev_term64(e, &in->c, x);
            if (x)
                sum += (u64)ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform) +
                       (u64)ev_pred(e, sm86_field(in, 77, 3), sm86_field(in, 80, 1), uniform);
            ev_dwr(e, in, (u32)sum);
            ev_pwr(e, p, (u32)(sum >> 32) > 0, uniform);
            ev_pwr(e, q, (u32)(sum >> 32) > 1, uniform);
            return 1;
        }
        case SM86_IMAD:
        case SM86_UIMAD: {
            /* with .X a negated addend is its complement, as IADD3.X's sources */
            u32 addend = sm86_field(in, 74, 1) && in->c.neg && in->c.kind != SM86_SRC_IMM ? ~ev_bits(e, &in->c)
                                                                                          : ev_int(e, &in->c);
            u32 v = ev_int(e, &in->a) * ev_int(e, &in->b) + addend;
            if (sm86_field(in, 74, 1)) v += (u32)ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            ev_dwr(e, in, v);
            return 1;
        }
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE: {
            u32 a = ev_int(e, &in->a), b = ev_int(e, &in->b);
            if (in->c.kind == SM86_SRC_IMM || in->c.kind == SM86_SRC_CBUF) return 0;
            u64 product = sm86_field(in, 73, 1) ? (u64)((i64)(i32)a * (i64)(i32)b) : (u64)a * b;
            u32 creg = in->c.reg, chi = in->c.kind == SM86_SRC_UREG ? (creg >= SM86_URZ ? 0 : e->ur[creg + 1])
                                                                    : (creg >= SM86_RZ ? 0 : e->r[creg + 1]);
            u64 sum = product + ((u64)chi << 32 | ev_bits(e, &in->c));
            int carry = sum < product; /* the carry out, and .X's carry in, as the translation's */
            if (sm86_field(in, 74, 1)) {
                u64 more = sum + (u64)ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
                carry |= more < sum;
                sum = more;
            }
            ev_pwr(e, sm86_field(in, 81, 3), carry, uniform);
            ev_dwr(e, in, (u32)sum);
            if (uniform && in->dst < SM86_URZ)
                ev_uwr(e, in->dst + 1, (u32)(sum >> 32));
            else if (!uniform && in->dst < SM86_RZ)
                ev_wr(e, in->dst + 1, (u32)(sum >> 32));
            return 1;
        }
        case SM86_LEA:
        case SM86_ULEA: {
            u32 shift = sm86_field(in, 75, 5), p = sm86_field(in, 81, 3), a = ev_bits(e, &in->a), shifted;
            if (sm86_field(in, 80, 1)) /* the high word of (c:a) << shift */
                shifted = shift ? ev_bits(e, &in->c) << shift | a >> (32 - shift) : ev_bits(e, &in->c);
            else
                shifted = a << shift;
            if (sm86_field(in, 72, 1)) shifted = 0u - shifted;
            u64 sum = (u64)shifted + ev_int(e, &in->b);
            if (sm86_field(in, 74, 1)) sum += (u64)ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            ev_dwr(e, in, (u32)sum);
            ev_pwr(e, p, (u32)(sum >> 32) > 0, uniform);
            return 1;
        }
        case SM86_LOP3:
        case SM86_ULOP3: {
            u32 v = ev_lut(sm86_field(in, 72, 8), ev_bits(e, &in->a), ev_bits(e, &in->b), ev_bits(e, &in->c));
            ev_dwr(e, in, v);
            ev_pwr(e, sm86_field(in, 81, 3), v != 0, uniform);
            return 1;
        }
        case SM86_SHF:
        case SM86_USHF: {
            u32 dtype = sm86_field(in, 73, 2), bits = dtype < 2 ? 64 : 32, s = ev_bits(e, &in->b);
            int right = (int)sm86_field(in, 76, 1), is_signed = dtype == 0 || dtype == 2;
            s = sm86_field(in, 75, 1) ? s & (bits - 1) : (s < bits ? s : bits);
            u64 value = (u64)ev_bits(e, &in->c) << 32 | ev_bits(e, &in->a), shifted;
            if (s >= 64)
                shifted = !right ? 0 : is_signed ? (u64)((i64)value >> 63) : 0;
            else
                shifted = !right ? value << s : is_signed ? (u64)((i64)value >> s) : value >> s;
            ev_dwr(e, in, sm86_field(in, 80, 1) ? (u32)(shifted >> 32) : (u32)shifted);
            return 1;
        }
        case SM86_SEL:
        case SM86_USEL:
            ev_dwr(e, in,
                   ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform) ? ev_bits(e, &in->a)
                                                                                     : ev_bits(e, &in->b));
            return 1;
        case SM86_IMNMX:
        case SM86_UIMNMX: {
            int min = ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 x = ev_bits(e, &in->a), y = ev_bits(e, &in->b);
            int less = sm86_field(in, 73, 1) ? (i32)x < (i32)y : x < y;
            ev_dwr(e, in, min == less ? x : y);
            return 1;
        }
        case SM86_ISETP:
        case SM86_UISETP: {
            /* no negation: bit 72 is .EX (the translation's ISETP) */
            u32 x = ev_bits(e, &in->a), y = ev_bits(e, &in->b), cmp_op = sm86_field(in, 76, 3);
            int cmp = ev_compare(cmp_op, (int)sm86_field(in, 73, 1), x, y);
            if (sm86_field(in, 72, 1) && cmp_op != 0 && cmp_op != 7 && x == y) /* .EX: the low words' comparison */
                cmp = ev_pred(e, sm86_field(in, 68, 3), sm86_field(in, 71, 1), uniform);
            int acc = ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 set_op = sm86_field(in, 74, 2);
            ev_pwr(e, sm86_field(in, 81, 3), ev_combine(set_op, cmp, acc), uniform);
            ev_pwr(e, sm86_field(in, 84, 3), ev_combine(set_op, !cmp, acc), uniform);
            return 1;
        }
        case SM86_PLOP3:
        case SM86_UPLOP3: {
            int u = op == SM86_UPLOP3;
            u32 x = (u32)ev_pred(e, sm86_field(in, 87, 3), sm86_field(in, 90, 1), u);
            u32 y = (u32)ev_pred(e, sm86_field(in, 77, 3), sm86_field(in, 80, 1), u);
            u32 z = (u32)ev_pred(e, sm86_field(in, 68, 3), sm86_field(in, 71, 1), u || (int)sm86_field(in, 67, 1));
            u32 lut0 = sm86_field(in, 64, 3) | sm86_field(in, 72, 5) << 3, lut1 = sm86_field(in, 16, 8);
            ev_pwr(e, sm86_field(in, 81, 3), (int)(ev_lut(lut0, x, y, z) & 1), u);
            ev_pwr(e, sm86_field(in, 84, 3), (int)(ev_lut(lut1, x, y, z) & 1), u);
            return 1;
        }
        case SM86_PRMT:
        case SM86_UPRMT: {
            u32 a = ev_bits(e, &in->a), sel = ev_bits(e, &in->b), hi = ev_bits(e, &in->c), v = 0;
            for (u32 k = 0; k < 4; k++) {
                u32 nib = sel >> (4 * k) & 15, byte = ((nib & 4 ? hi : a) >> ((nib & 3) * 8)) & 0xff;
                if (nib & 8) byte = byte & 0x80 ? 0xff : 0;
                v |= byte << (8 * k);
            }
            ev_dwr(e, in, v);
            return 1;
        }
        case SM86_ULDC:
        case SM86_ULDC_IDX: {
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            u32 index = 0, d = sm86_field(in, 16, 6);
            if (op == SM86_ULDC_IDX) {
                u32 ur = sm86_field(in, 24, 6);
                index = ur < SM86_URZ ? e->ur[ur] : 0;
                if (off & 0x8000u) off |= 0xffff0000u;
            }
            ev_uwr(e, d, ev_cbuf(e, bank, off, index, type, 0));
            if (type == 5 && d < SM86_URZ) ev_uwr(e, d + 1, ev_cbuf(e, bank, off, index, type, 1));
            return 1;
        }
        case SM86_LDC: {
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            u32 r = sm86_field(in, 24, 8), index = r < SM86_RZ ? e->r[r] : 0;
            ev_wr(e, in->dst, ev_cbuf(e, bank, off, index, type, 0));
            if (type == 5 && in->dst < SM86_RZ) ev_wr(e, in->dst + 1, ev_cbuf(e, bank, off, index, type, 1));
            return 1;
        }
        case SM86_LDG:
        case SM86_LDG_NOU: {
            /* the address as global_dynamic makes it: a register (a pair with bit 90 for the uniform form, 72 for
               the other), the uniform form's uniform register (a pair with bit 72), the signed 24-bit offset */
            int uniform_form = op == SM86_LDG;
            u32 type = sm86_field(in, 73, 3), words = type == 5 ? 2 : type == 6 ? 4 : 1, w[4] = {0, 0, 0, 0};
            u32 reg = sm86_field(in, 24, 8), imm = sm86_field(in, 40, 24);
            u64 address = reg < SM86_RZ ? e->r[reg] : 0;
            if (sm86_field(in, uniform_form ? 90 : 72, 1) && reg < SM86_RZ) address |= (u64)e->r[reg + 1] << 32;
            address += imm & 0x800000u ? (u64)imm | 0xffffffffff000000ull : imm;
            if (uniform_form) {
                u32 ur = sm86_field(in, 32, 6);
                if (ur < SM86_URZ)
                    address += (sm86_field(in, 72, 1) && ur + 1 < SM86_URZ ? (u64)e->ur[ur + 1] << 32 : 0) | e->ur[ur];
            }
            if (!e->io->mem(e->io->user, address & ~3ull, w, words)) return 0;
            for (u32 k = 0; k < words; k++) ev_wr(e, in->dst >= SM86_RZ ? SM86_RZ : in->dst + k,
                                                  type < 4 ? ev_subword(w[k], (u32)address, type) : w[k]);
            return 1;
        }
        default: return 0;
    }
}

int sm86_eval(const u64 (*insn)[2], u32 n, const Sm86EvalIo* io, const u32* probes, u32 nprobes, u32* out) {
    Ev e;
    memset(&e, 0, sizeof e);
    e.io = io;
    for (u32 k = 0; k < nprobes; k++) out[k] = 0;
    for (u32 i = 0; i < n; i++) {
        Sm86Insn in;
        if (!sm86_decode(insn[i][0], insn[i][1], &in) || !sm86_eval_takes(&in) || !ev_step(&e, &in)) return 0;
        for (u32 k = 0; k < nprobes; k++) {
            if (SH_PROBE_INSN(probes[k]) != i) continue;
            u32 r = SH_PROBE_REG(probes[k]);
            out[k] = SH_PROBE_UNIFORM(probes[k]) ? (r < SM86_URZ ? e.ur[r] : 0) : (r < SM86_RZ ? e.r[r] : 0);
        }
    }
    return 1;
}
