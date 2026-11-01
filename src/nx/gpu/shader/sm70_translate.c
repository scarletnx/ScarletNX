/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The operand order of the texture instructions is that of Mesa's NAK (src/nouveau/compiler/nak_nir_lower_tex.c):
   Copyright © 2023 Collabora, Ltd.
   SPDX-License-Identifier: MIT */
/* SM70+ programs to SPIR-V (sm70.h). The program's registers, uniform registers and predicates are Function
   variables; constant buffers, global buffers, textures and attributes are the resources of the Maxwell translator's
   layout (shader.h), declared when an instruction first uses them, so that the renderer binds both alike.

   Control flow is the program's own branches. BSSY, BSYNC, BREAK and WARPSYNC only manage the convergence of the
   warp, so the translation reads past them, and structure.c (shader_structure_graph) places the blocks that BRA and
   EXIT end into structured constructs. structure_step follows spirv_translate.c's, which takes its own context. */
#include "nx/gpu/shader/sm70.h"
#include "nx/gpu/shader/translator.h"
#include "common/utilities/spirv_emit.h"
#include <stdarg.h>

#define MAX_INTERFACES 256

typedef struct {
    SpvEmitter e;
    ShaderInfo* si;
    int stage;
    const u32* sph;
    const Sm70Insn* in;
    u32 n;
    /* types and constants */
    u32 t_void, t_bool, t_u32, t_i32, t_f32, t_v2f, t_v3f, t_v4f, t_v4u, t_v3u, t_v2u;
    u32 t_u64_; /* made with the Int64 capability when an instruction needs it (u64_type) */
    u32 zero, true_v, false_v, glsl;
    /* the program's state: Function variables, made at the start */
    u32 reg[256], ureg[64], pred[8], upred[8];
    /* resources, made when first used */
    u32 cbuf[18], cbuf_ptr;
    u32 gbuf[16], gbuf_ptr;
    u32 shared, shared_ptr, shared_words;
    u32 in_attr[32], out_attr[32], position, point_size, fragcoord, color[8], depth;
    u32 vertex_index, instance_index, local_id, group_id, lane_id;
    u32 push, push_ptr; /* a vertex program's push constant vec4: the base instance, the depth fix (vk/render.c) */
    u32 centroid_mask;  /* the inputs decorated Centroid */
    u32 point_coord, front_facing;
    u32 tex[SH_MAXTEX], tex_sampled[SH_MAXTEX];
    u32 img[8], img_type[8];
    u32 interfaces[MAX_INTERFACES], ninterfaces;
    /* the uniform register pair a ULDC.64 last loaded from a constant buffer: (bank << 16 | offset) + 1, 0 for none */
    u32* addr_key;   /* by instruction: the constant buffer word (bank << 16 | offset) + 1 of an LDG's or STG's base */
    u32 cur;         /* the instruction being written */
    u32 branch_cond; /* the condition of the BRA that ends the body before, 0 for always */
    u32 branch_target; /* the instruction that the BRX which ends the body before goes to */
    int failed;
} Ctx;

static void fail(Ctx* c, const char* fmt, ...) {
    if (c->failed) return;
    c->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->si->first_unsupported, sizeof c->si->first_unsupported, fmt, ap);
    va_end(ap);
    c->si->unsupported++;
}
static void add_interface(Ctx* c, u32 id) {
    if (c->ninterfaces < MAX_INTERFACES)
        c->interfaces[c->ninterfaces++] = id;
    else
        fail(c, "too many interface variables");
}
static u32 cu(Ctx* c, u32 v) { return spv_emit_constant_u32(&c->e, c->t_u32, v); }
static u32 cf(Ctx* c, float v) { return spv_emit_constant_f32(&c->e, c->t_f32, v); }
static u32 to_f(Ctx* c, u32 v) { return spv_emit_bitcast(&c->e, c->t_f32, v); }
static u32 to_u(Ctx* c, u32 v) { return spv_emit_bitcast(&c->e, c->t_u32, v); }
static u32 op2(Ctx* c, u32 opcode, u32 type, u32 a, u32 b) { return SPV(&c->e, opcode, type, a, b); }

/* ---- registers ---- */

static u32 rd(Ctx* c, u32 r) { return r >= SM70_RZ ? c->zero : spv_emit_load(&c->e, c->t_u32, c->reg[r]); }
static void wr(Ctx* c, u32 r, u32 v) {
    if (r < SM70_RZ) spv_emit_store(&c->e, c->reg[r], v);
}
static u32 urd(Ctx* c, u32 r) { return r >= SM70_URZ ? c->zero : spv_emit_load(&c->e, c->t_u32, c->ureg[r]); }
static void uwr(Ctx* c, u32 r, u32 v) {
    if (r < SM70_URZ) { spv_emit_store(&c->e, c->ureg[r], v); }
}
static u32 prd(Ctx* c, u32 p, u32 negate, int uniform) {
    u32 v = p >= SM70_PT ? c->true_v : spv_emit_load(&c->e, c->t_bool, uniform ? c->upred[p] : c->pred[p]);
    return negate ? spv_emit_bool_not(&c->e, c->t_bool, v) : v;
}
static void pwr(Ctx* c, u32 p, u32 v, int uniform) {
    if (p < SM70_PT) spv_emit_store(&c->e, uniform ? c->upred[p] : c->pred[p], v);
}
/* the destination of an ALU instruction, a register or a uniform register */
static void dwr(Ctx* c, const Sm70Insn* in, u32 v) {
    if (sm70_ops[in->op].kind == SM70_K_UALU)
        uwr(c, in->dst, v);
    else
        wr(c, in->dst, v);
}

/* ---- constant buffers ---- */

static u32 cbuf_var(Ctx* c, u32 bank, u32 end) {
    if (bank >= 18) {
        fail(c, "constant buffer %u", bank);
        return 0;
    }
    if (!c->cbuf[bank]) {
        c->cbuf[bank] = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                                    SNX_VKB_CB + bank, 4096, &c->cbuf_ptr);
        add_interface(c, c->cbuf[bank]);
        c->si->cbuf_mask |= 1u << bank;
    }
    if (end > c->si->cbuf_max[bank]) c->si->cbuf_max[bank] = end > 0x10000 ? 0x10000 : end;
    return c->cbuf[bank];
}
/* the word at byte offset off of a bank, plus the register value index when it isn't 0 */
static u32 cbuf_load(Ctx* c, u32 bank, u32 off, u32 index) {
    u32 var = cbuf_var(c, bank, index ? 0x10000 : off + 4);
    if (!var) return c->zero;
    u32 element, component;
    if (index) {
        u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off));
        element = spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 4)),
                                           cu(c, 4095));
        component = spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 2)),
                                             cu(c, 3));
    } else {
        element = cu(c, (off >> 4) & 4095);
        component = cu(c, (off >> 2) & 3);
    }
    return spv_emit_uniform_component(&c->e, c->cbuf_ptr, var, c->zero, element, component, c->t_u32);
}

/* ---- ALU sources ---- */

static u32 src_bits(Ctx* c, const Sm70Src* s) {
    switch (s->kind) {
        case SM70_SRC_REG: return rd(c, s->reg);
        case SM70_SRC_UREG: return urd(c, s->reg);
        case SM70_SRC_IMM: return cu(c, s->value);
        case SM70_SRC_CBUF:
            if (s->bindless) {
                fail(c, "bindless constant buffer operand");
                return c->zero;
            }
            return cbuf_load(c, s->bank, s->value, 0);
        default: return c->zero;
    }
}
/* an integer source with its negation */
static u32 src_int(Ctx* c, const Sm70Src* s) {
    u32 v = src_bits(c, s);
    return s->neg && s->kind != SM70_SRC_IMM ? spv_emit_isub_u32(&c->e, c->t_u32, c->zero, v) : v;
}
/* a float source with its absolute value and negation */
static u32 src_f(Ctx* c, const Sm70Src* s) {
    u32 v = to_f(c, src_bits(c, s));
    if (s->kind == SM70_SRC_IMM) return v;
    if (s->abs) v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, v);
    if (s->neg) v = spv_emit_fnegate(&c->e, c->t_f32, v);
    return v;
}
/* the second source of a two-source ALU instruction: b, or c for the forms that put a register in b's place and the
   operand in c's (FADD's register forms keep it in b) */
static const Sm70Src* second(const Sm70Insn* in) {
    return in->form == 2 || in->form == 3 || in->form == 7 ? &in->c : &in->b;
}
static u32 saturate(Ctx* c, u32 v, int on) {
    if (!on) return v;
    /* NaN gives 0, as the hardware's .SAT */
    u32 positive = spv_emit_fcompare(&c->e, c->t_bool, 4, v, cf(c, 0.0f)); /* NAK's FloatCmpOp: 4 >, 6 >= */
    u32 low = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_MIN, v, cf(c, 1.0f));
    return spv_emit_select(&c->e, c->t_f32, positive, low, cf(c, 0.0f));
}
static u32 combine(Ctx* c, u32 set_op, u32 x, u32 acc) {
    switch (set_op) {
        case 0: return op2(c, SPV_OP_LOGICAL_AND, c->t_bool, x, acc);
        case 1: return op2(c, SPV_OP_LOGICAL_OR, c->t_bool, x, acc);
        default: return op2(c, SPV_OP_LOGICAL_NOT_EQUAL, c->t_bool, x, acc);
    }
}
/* NAK's IntCmpOp, bits 76-78 */
static u32 int_compare(Ctx* c, u32 cmp, int is_signed, u32 x, u32 y) {
    static const u16 u_ops[8] = {0,
                                 SPV_OP_U_LESS_THAN,
                                 SPV_OP_I_EQUAL,
                                 SPV_OP_U_LESS_THAN_EQUAL,
                                 SPV_OP_U_GREATER_THAN,
                                 SPV_OP_I_NOT_EQUAL,
                                 SPV_OP_U_GREATER_THAN_EQUAL,
                                 0};
    static const u16 s_ops[8] = {0,
                                 SPV_OP_S_LESS_THAN,
                                 SPV_OP_I_EQUAL,
                                 SPV_OP_S_LESS_THAN_EQUAL,
                                 SPV_OP_S_GREATER_THAN,
                                 SPV_OP_I_NOT_EQUAL,
                                 SPV_OP_S_GREATER_THAN_EQUAL,
                                 0};
    if (cmp == 0) return c->false_v;
    if (cmp == 7) return c->true_v;
    return op2(c, is_signed ? s_ops[cmp] : u_ops[cmp], c->t_bool, x, y);
}
/* a 3-input truth table (LOP3, PLOP3) over three values of type, x the 0xf0 input, y 0xcc and z 0xaa */
static u32 lut3(Ctx* c, u32 lut, u32 type, u32 x, u32 y, u32 z, u32 all, u32 none) {
    int boolean = type == c->t_bool;
    u32 and_op = boolean ? SPV_OP_LOGICAL_AND : SPV_OP_BITWISE_AND,
        or_op = boolean ? SPV_OP_LOGICAL_OR : SPV_OP_BITWISE_OR;
    u32 not_op = boolean ? SPV_OP_LOGICAL_NOT : SPV_OP_NOT;
    switch (lut & 0xff) {
        case 0x00: return none;
        case 0xff: return all;
        case 0xf0: return x;
        case 0xcc: return y;
        case 0xaa: return z;
        case 0xc0: return op2(c, and_op, type, x, y);
        case 0xa0: return op2(c, and_op, type, x, z);
        case 0x88: return op2(c, and_op, type, y, z);
        case 0xfc: return op2(c, or_op, type, x, y);
        case 0xfa: return op2(c, or_op, type, x, z);
        case 0xee: return op2(c, or_op, type, y, z);
        default: break;
    }
    u32 nx = SPV(&c->e, not_op, type, x), ny = SPV(&c->e, not_op, type, y), nz = SPV(&c->e, not_op, type, z);
    u32 result = 0;
    for (u32 i = 0; i < 8; i++) {
        if (!(lut & (1u << i))) continue;
        u32 term = op2(c, and_op, type, op2(c, and_op, type, i & 4 ? x : nx, i & 2 ? y : ny), i & 1 ? z : nz);
        result = result ? op2(c, or_op, type, result, term) : term;
    }
    return result;
}

/* ---- the 64-bit helpers of IADD3, LEA and SHF ---- */

static u32 u64_type(Ctx* c) {
    if (!c->t_u64_) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_INT64);
        c->t_u64_ = spv_emit_type_int64(&c->e, 0);
    }
    return c->t_u64_;
}
static u32 c64(Ctx* c, u64 v) { return spv_emit_constant64(&c->e, u64_type(c), v); }

static u32 to64(Ctx* c, u32 v) { return SPV(&c->e, SPV_OP_U_CONVERT, u64_type(c), v); }
static u32 lo32(Ctx* c, u32 v) { return SPV(&c->e, SPV_OP_U_CONVERT, c->t_u32, v); }
static u32 hi32(Ctx* c, u32 v) { return lo32(c, op2(c, SPV_OP_SHIFT_RIGHT_LOGICAL, u64_type(c), v, c64(c, 32))); }
static u32 pair64(Ctx* c, u32 low, u32 high) {
    return op2(c, SPV_OP_BITWISE_OR, u64_type(c), to64(c, low),
               op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, u64_type(c), to64(c, high), c64(c, 32)));
}
static u32 carry_in(Ctx* c, u32 field_lo, const Sm70Insn* in, int uniform) {
    u32 p = prd(c, sm70_field(in, field_lo, 3), sm70_field(in, field_lo + 3, 1), uniform);
    return spv_emit_select(&c->e, u64_type(c), p, c64(c, 1), c64(c, 0));
}
/* a source of a 64-bit sum: its two's complement as NAK's fold reads it, ~x + 1 in 33 bits */
static u32 term64(Ctx* c, const Sm70Src* s) {
    u32 v = src_bits(c, s);
    if (!s->neg || s->kind == SM70_SRC_IMM) return to64(c, v);
    return op2(c, SPV_OP_I_ADD, u64_type(c), to64(c, spv_emit_not_u32(&c->e, c->t_u32, v)), c64(c, 1));
}

/* ---- FTZ and the rounding modes ---- */

/* An FTZ instruction (bit 80) reads and writes a subnormal as the zero of its sign: per operand and result, as
   spirv_translate.c's float_ftz does in SH_FTZ_EMULATE */
static u32 ftz(Ctx* c, u32 v, int on) {
    return on ? spv_emit_flush_subnormal(&c->e, c->t_f32, c->t_u32, c->t_bool, c->glsl, v, 1) : v;
}
static void note_ftz(Ctx* c, int on) {
    if (on)
        c->si->fp32_flush = 1;
    else
        c->si->fp32_preserve = 1;
}
static u32 f64_type(Ctx* c) {
    spv_emit_float64_capability(&c->e);
    return spv_emit_type_f64(&c->e);
}
static u32 nocontract(Ctx* c, u32 v) {
    spv_emit_no_contraction(&c->e, v);
    return v;
}
/* NAK's FRndMode (bits 78-79): 0 to nearest even, 1 toward -inf, 2 toward +inf, 3 toward zero. r is the result
   rounded to nearest, and above and below say if the exact result is above or below it. A directed mode moves r
   one step (one unit of its bits) when the exact result is on the other side of it */
static u32 directed(Ctx* c, u32 mode, u32 r, u32 above, u32 below) {
    if (mode == 0) return r;
    u32 bits = to_u(c, r);
    u32 negative = spv_emit_u_less(&c->e, c->t_bool, cu(c, 0x7fffffffu), bits);
    u32 positive = spv_emit_bool_not(&c->e, c->t_bool, negative);
    u32 up, down; /* toward +inf, toward -inf */
    if (mode == 1) {
        up = c->false_v;
        down = below;
    } else if (mode == 2) {
        up = above;
        down = c->false_v;
    } else {
        up = op2(c, SPV_OP_LOGICAL_AND, c->t_bool, negative, above);
        down = op2(c, SPV_OP_LOGICAL_AND, c->t_bool, positive, below);
    }
    /* a step up adds one to a positive value's bits and takes one from a negative one's, and -0 goes to the
       smallest positive subnormal. A step down is the mirror of it */
    u32 plus = spv_emit_iadd_u32(&c->e, c->t_u32, bits, cu(c, 1));
    u32 minus = spv_emit_isub_u32(&c->e, c->t_u32, bits, cu(c, 1));
    u32 is_neg_zero = spv_emit_i_equal(&c->e, c->t_bool, bits, cu(c, 0x80000000u));
    u32 is_pos_zero = spv_emit_i_equal(&c->e, c->t_bool, bits, c->zero);
    u32 up_bits = spv_emit_select(&c->e, c->t_u32, positive, plus,
                                  spv_emit_select(&c->e, c->t_u32, is_neg_zero, cu(c, 1), minus));
    u32 down_bits = spv_emit_select(&c->e, c->t_u32, negative, plus,
                                    spv_emit_select(&c->e, c->t_u32, is_pos_zero, cu(c, 0x80000001u), minus));
    bits = spv_emit_select(&c->e, c->t_u32, up, up_bits, spv_emit_select(&c->e, c->t_u32, down, down_bits, bits));
    return to_f(c, bits);
}
static u32 widen(Ctx* c, u32 f) { return SPV(&c->e, SPV_OP_F_CONVERT, f64_type(c), f); }
/* a double rounded to a float by mode: exact when the double holds the exact result */
static u32 round_f64(Ctx* c, u32 mode, u32 d) {
    u32 r = SPV(&c->e, SPV_OP_F_CONVERT, c->t_f32, d);
    if (mode == 0) return r;
    u32 back = widen(c, r);
    return directed(c, mode, r, op2(c, SPV_OP_F_ORD_GREATER_THAN, c->t_bool, d, back),
                    op2(c, SPV_OP_F_ORD_LESS_THAN, c->t_bool, d, back));
}
/* The exact sum of two doubles p and q, which are floats or a product of two, rounded to a float by mode. TwoSum
   gives the sum s to nearest and its error e exactly: in doubles, which no float operand makes subnormal. The float
   nearest s is one of the two around the exact sum (s is the double nearest it), and e breaks a tie at s */
static u32 round_sum(Ctx* c, u32 mode, u32 p, u32 q) {
    u32 f64 = f64_type(c);
    u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, f64, p, q));
    u32 bb = nocontract(c, op2(c, SPV_OP_F_SUB, f64, sum, p));
    u32 ea = nocontract(c, op2(c, SPV_OP_F_SUB, f64, p, nocontract(c, op2(c, SPV_OP_F_SUB, f64, sum, bb))));
    u32 err = nocontract(c, op2(c, SPV_OP_F_ADD, f64, ea, nocontract(c, op2(c, SPV_OP_F_SUB, f64, q, bb))));
    u32 r = SPV(&c->e, SPV_OP_F_CONVERT, c->t_f32, sum), back = widen(c, r);
    u32 zero = spv_emit_constant64(&c->e, f64, 0);
    u32 tie = op2(c, SPV_OP_F_ORD_EQUAL, c->t_bool, sum, back);
    u32 above =
        op2(c, SPV_OP_LOGICAL_OR, c->t_bool, op2(c, SPV_OP_F_ORD_GREATER_THAN, c->t_bool, sum, back),
            op2(c, SPV_OP_LOGICAL_AND, c->t_bool, tie, op2(c, SPV_OP_F_ORD_GREATER_THAN, c->t_bool, err, zero)));
    u32 below = op2(c, SPV_OP_LOGICAL_OR, c->t_bool, op2(c, SPV_OP_F_ORD_LESS_THAN, c->t_bool, sum, back),
                    op2(c, SPV_OP_LOGICAL_AND, c->t_bool, tie, op2(c, SPV_OP_F_ORD_LESS_THAN, c->t_bool, err, zero)));
    return directed(c, mode, r, above, below);
}
/* FADD: x + y in mode */
static u32 float_add(Ctx* c, u32 mode, u32 x, u32 y) {
    if (mode == 0) return nocontract(c, spv_emit_fadd(&c->e, c->t_f32, x, y));
    return round_sum(c, mode, widen(c, x), widen(c, y));
}
/* FMUL: x * y * 2^(scale - 4) in mode. The product of two floats is exact in a double */
static u32 float_mul(Ctx* c, u32 mode, u32 scale, u32 x, u32 y) {
    if (mode == 0 && scale == 4) return nocontract(c, spv_emit_fmul(&c->e, c->t_f32, x, y));
    u32 f64 = f64_type(c);
    u32 d = nocontract(c, op2(c, SPV_OP_F_MUL, f64, widen(c, x), widen(c, y)));
    if (scale != 4) {
        u64 factor = (u64)(1023 + scale - 4) << 52; /* 2^(scale - 4), scale 1 to 7 */
        d = nocontract(c, op2(c, SPV_OP_F_MUL, f64, d, spv_emit_constant64(&c->e, f64, factor)));
    }
    return round_f64(c, mode, d);
}
/* FFMA: x * y + z in mode. To nearest, a float FMA, as spirv_translate.c writes it. A directed mode rounds the
   exact sum of the product (exact in a double) and z */
static u32 float_fma(Ctx* c, u32 mode, u32 x, u32 y, u32 z) {
    if (mode == 0) return spv_emit_glsl_ternary(&c->e, c->t_f32, c->glsl, SPV_GLSL_FMA, x, y, z);
    u32 product = nocontract(c, op2(c, SPV_OP_F_MUL, f64_type(c), widen(c, x), widen(c, y)));
    return round_sum(c, mode, product, widen(c, z));
}

/* ---- a vertex program's push constant ---- */

/* component k of the vec4 at push constant offset 0, which the renderer gives a vertex program as it gives the
   Maxwell one (spirv_translate.c): x the base instance's bits, y not 0 when the depth fix is on */
static u32 push_component(Ctx* c, u32 k) {
    if (!c->push) {
        c->push = spv_emit_push_vec4(&c->e, c->t_v4f, c->t_f32, 0, 0, &c->push_ptr);
        add_interface(c, c->push);
    }
    return spv_emit_push_component_f32(&c->e, c->push_ptr, c->push, c->zero, cu(c, k), c->t_f32);
}
/* The depth fix of spirv_translate.c's vertex_depth_fix: with the push constant's y not 0, z becomes (z + w) / 2,
   from the console's clip space depth of -w to w to Vulkan's of 0 to w */
static void vertex_depth_fix(Ctx* c) {
    u32 enabled = spv_emit_fcompare(&c->e, c->t_bool, 5, push_component(c, 1), cf(c, 0.0f)); /* 5: != */
    u32 adjust = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    spv_emit_selection_merge(&c->e, merge);
    spv_emit_branch_conditional(&c->e, enabled, adjust, merge);
    spv_emit_label(&c->e, adjust);
    u32 value = spv_emit_load(&c->e, c->t_v4f, c->position);
    u32 sum = nocontract(c, spv_emit_fadd(&c->e, c->t_f32, spv_emit_vec4_extract(&c->e, c->t_f32, value, 2),
                                          spv_emit_vec4_extract(&c->e, c->t_f32, value, 3)));
    u32 depth = nocontract(c, spv_emit_fmul(&c->e, c->t_f32, sum, cf(c, 0.5f)));
    spv_emit_store(&c->e, c->position, spv_emit_vec4_insert(&c->e, c->t_v4f, value, depth, 2));
    spv_emit_branch(&c->e, merge);
    spv_emit_label(&c->e, merge);
}
static void position_var(Ctx* c) {
    if (!c->position) {
        c->position = spv_emit_position_output(&c->e, c->t_v4f);
        add_interface(c, c->position);
    }
}

/* a float to BF16 bits, rounded to nearest even, a NaN kept a NaN */
static u32 bf16(Ctx* c, u32 f) {
    u32 bits = to_u(c, f);
    u32 odd =
        spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, bits, cu(c, 16)), cu(c, 1));
    u32 rounded = spv_emit_shift_right_u32(
        &c->e, c->t_u32,
        spv_emit_iadd_u32(&c->e, c->t_u32, bits, spv_emit_iadd_u32(&c->e, c->t_u32, cu(c, 0x7fff), odd)), cu(c, 16));
    u32 nan = spv_emit_isnan(&c->e, c->t_bool, f);
    return spv_emit_select(&c->e, c->t_u32, nan,
                           spv_emit_bitwise_or_u32(&c->e, c->t_u32,
                                                   spv_emit_shift_right_u32(&c->e, c->t_u32, bits, cu(c, 16)),
                                                   cu(c, 0x40)),
                           spv_emit_bitwise_and_u32(&c->e, c->t_u32, rounded, cu(c, 0xffff)));
}
/* ---- attributes ---- */

static u32 interp_mode(Ctx* c, u32 input) {
    /* the SPH's imap of a fragment program: 2 bits a component, 1 constant, 2 perspective, 3 screen linear, its first
       used component's, and perspective for none, as spirv_translate.c reads Maxwell's (the same layout) */
    for (u32 k = 0; k < 4; k++) {
        u32 mode = (c->sph[6 + input / 4] >> ((input % 4) * 8 + k * 2)) & 3;
        if (mode) return mode;
    }
    return 2;
}
static u32 input_var(Ctx* c, u32 i) {
    if (!c->in_attr[i]) {
        int interpolation = 0;
        if (c->stage == ST_FS) {
            u32 mode = interp_mode(c, i);
            interpolation = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        }
        u32 type = c->t_v4f;
        if (c->stage == ST_VS && (c->si->attr_int_mask & (1u << i)))
            type = c->si->attr_sint_mask & (1u << i) ? spv_emit_type_vec4(&c->e, c->t_i32) : c->t_v4u;
        c->in_attr[i] = spv_emit_input_vec4(&c->e, type, i, interpolation);
        add_interface(c, c->in_attr[i]);
        c->si->in_mask |= 1u << i;
    }
    return c->in_attr[i];
}
/* the bits of component k of generic input i */
static u32 input_load(Ctx* c, u32 i, u32 k) {
    u32 var = input_var(c, i);
    int integer = c->stage == ST_VS && (c->si->attr_int_mask & (1u << i));
    u32 scalar = integer ? (c->si->attr_sint_mask & (1u << i) ? c->t_i32 : c->t_u32) : c->t_f32;
    u32 ptr = spv_emit_interface_scalar_pointer(&c->e, 1, scalar, var, 0, cu(c, k));
    return to_u(c, spv_emit_load(&c->e, scalar, ptr));
}
static u32 builtin_u32(Ctx* c, u32* var, u32 builtin) {
    if (!*var) {
        *var = spv_emit_builtin_input_u32(&c->e, c->t_u32, builtin);
        add_interface(c, *var);
        if (c->stage == ST_FS) spv_emit_flat(&c->e, *var);
    }
    return spv_emit_load(&c->e, c->t_u32, *var);
}
static u32 builtin_vec3(Ctx* c, u32* var, u32 builtin, u32 k) {
    if (!*var) {
        *var = spv_emit_builtin_input_vec3(&c->e, c->t_v3u, builtin);
        add_interface(c, *var);
    }
    return spv_emit_vec4_extract(&c->e, c->t_u32, spv_emit_load(&c->e, c->t_v3u, *var), k);
}
static u32 frag_coord(Ctx* c, u32 k) {
    if (!c->fragcoord) {
        c->fragcoord = spv_emit_fragcoord_input(&c->e, c->t_v4f);
        add_interface(c, c->fragcoord);
    }
    return spv_emit_vec4_extract(&c->e, c->t_f32, spv_emit_load(&c->e, c->t_v4f, c->fragcoord), k);
}
/* ALD's attribute at byte address addr, as bits */
static u32 attr_load(Ctx* c, u32 addr) {
    if (addr >= 0x80 && addr < 0x280) return input_load(c, (addr - 0x80) >> 4, (addr >> 2) & 3);
    if (c->stage == ST_VS && addr == 0x2f8) /* InstanceIndex counts from the base instance, the push constant's x */
        return spv_emit_isub_u32(&c->e, c->t_u32, builtin_u32(c, &c->instance_index, 43),
                                 to_u(c, push_component(c, 0)));
    if (c->stage == ST_VS && addr == 0x2fc) return builtin_u32(c, &c->vertex_index, 42);
    if (c->stage == ST_FS && addr >= 0x70 && addr < 0x80) return to_u(c, frag_coord(c, (addr >> 2) & 3));
    fail(c, "attribute load 0x%x", addr);
    return c->zero;
}
static void attr_store(Ctx* c, u32 addr, u32 bits) {
    u32 var = 0, k = (addr >> 2) & 3;
    if (c->stage == ST_FS || c->stage == ST_CS) {
        fail(c, "attribute store in a fragment or compute program");
        return;
    }
    if (c->stage != ST_VS) {
        fail(c, "attribute store of stage %d", c->stage);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        position_var(c);
        var = c->position;
    } else if (addr == 0x5c) {
        /* NAK's NAK_ATTR_VPRS_TABLE_INDEX from SM86, the primitive's shading rate: the default rate, one fragment
           a pixel, is what the translation gives without it */
        return;
    } else if (addr == 0x6c) {
        if (!c->point_size) {
            c->point_size = spv_emit_point_size_output(&c->e, c->t_f32);
            add_interface(c, c->point_size);
        }
        spv_emit_store(&c->e, c->point_size, to_f(c, bits));
        c->si->writes_point_size = 1;
        return;
    } else if (addr >= 0x80 && addr < 0x280) {
        u32 i = (addr - 0x80) >> 4;
        if (!c->out_attr[i]) {
            c->out_attr[i] = spv_emit_output_vec4(&c->e, c->t_v4f, i);
            add_interface(c, c->out_attr[i]);
            c->si->out_mask |= 1u << i;
        }
        var = c->out_attr[i];
    } else {
        fail(c, "attribute store 0x%x", addr);
        return;
    }
    spv_emit_store(&c->e, spv_emit_interface_scalar_pointer(&c->e, 3, c->t_f32, var, 0, cu(c, k)), to_f(c, bits));
}

/* ---- global and shared memory ---- */

static u32 gbuf_slot(Ctx* c, u32 key, int write) {
    int slot = -1;
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) slot = i;
    if (slot < 0) {
        if (c->si->ngbuf >= 16) {
            fail(c, "more than 16 global buffers");
            return 0;
        }
        slot = c->si->ngbuf++;
        c->si->gbuf_cbuf_off[slot] = key;
    }
    if (!c->gbuf[slot]) {
        u32 array = spv_emit_type_runtime_array(&c->e, c->t_u32, 4);
        u32 structure = spv_emit_type_struct_one(&c->e, array);
        c->gbuf_ptr = spv_emit_type_storage_pointer(&c->e, c->t_u32);
        c->gbuf[slot] =
            spv_emit_storage_buffer(&c->e, structure, (u32)shader_vk_set(c->stage), SNX_VKB_SSBO + (u32)slot);
        add_interface(c, c->gbuf[slot]);
    }
    if (write) c->si->gbuf_wmask |= 1u << slot;
    return c->gbuf[slot];
}
/* LDG and STG, as spirv_translate.c's global memory: the buffer is the one whose 64-bit address the constant buffer
   word that track_globals found holds (ShaderInfo.gbuf_cbuf_off), and the byte offset into it is the address less
   that word, read at run time. The address is a register (a pair with bit 90 for the uniform form, bit 72 for the
   other) and a uniform register pair (bit 72), with a signed 24-bit offset; the low 32 bits of the difference serve */
static u32 global_address(Ctx* c, const Sm70Insn* in, int uniform_form, int write, u32* offset) {
    u32 key = c->addr_key[c->cur];
    if (!key) {
        u32 src = c->addr_key[c->n + 1 + c->cur];
        fail(c, "global address whose base no constant buffer holds (from %s, at %u)",
             src ? sm70_ops[src].name : "nothing", c->cur);
        return 0;
    }
    key--;
    u32 imm = sm70_field(in, 40, 24), reg = sm70_field(in, 24, 8);
    u32 bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, rd(c, reg), cu(c, bias));
    if (uniform_form) {
        u32 ur = sm70_field(in, in->op == SM70_LDG ? 32 : 64, 6);
        if (ur < SM70_URZ) at = spv_emit_iadd_u32(&c->e, c->t_u32, at, urd(c, ur));
    }
    u32 base = cbuf_load(c, SH_GBUF_BANK(key), SH_GBUF_OFFSET(key), 0);
    *offset = spv_emit_isub_u32(&c->e, c->t_u32, at, base);
    return gbuf_slot(c, key, write);
}
/* The constant buffer word that holds the base of each LDG's and STG's address, in program order: a register
   gets the word that ULDC.64, LDC.64 or MOV loads into it, and keeps it through moves and through IADD3, IMAD,
   IMAD.WIDE and LEA with one source that has it, as Eden's Track follows Maxwell's */
static int track_globals(Ctx* c) {
    u32 rkey[256] = {0}, ukey[64] = {0};
    /* the keys, then the opcode that last wrote each address register (for the message of an address without one) */
    c->addr_key = calloc(2 * ((size_t)c->n + 1), sizeof *c->addr_key);
    u16 rsrc[256] = {0}, usrc[64] = {0};
    if (!c->addr_key) return 0;
    for (u32 i = 0; i < c->n; i++) {
        const Sm70Insn* in = &c->in[i];
        int op = in->op, uniform = sm70_ops[op].kind == SM70_K_UALU;
        if (op == SM70_LDG || op == SM70_STG || op == SM70_LDG_NOU || op == SM70_STG_NOU || op == SM70_ATOMG ||
            op == SM70_ATOMG_NOU || op == SM70_RED) {
            int uniform_form = op == SM70_LDG || op == SM70_STG || op == SM70_ATOMG || op == SM70_RED;
            u32 reg = sm70_field(in, 24, 8), ur = uniform_form ? sm70_field(in, op == SM70_LDG ? 32 : 64, 6) : SM70_URZ;
            u32 k = ur < SM70_URZ && ukey[ur] ? ukey[ur] : reg < SM70_RZ ? rkey[reg] : 0;
            c->addr_key[i] = k;
            c->addr_key[c->n + 1 + i] = ur < SM70_URZ && ukey[ur] ? usrc[ur] : reg < SM70_RZ ? rsrc[reg] : 0;
            if (!k && ur < SM70_URZ && !ukey[ur] && usrc[ur]) c->addr_key[c->n + 1 + i] = usrc[ur];
            if (op == SM70_LDG || op == SM70_LDG_NOU || op == SM70_ATOMG || op == SM70_ATOMG_NOU) {
                /* the words loaded are data: one, or two or four for an LDG.64 or .128 */
                u32 type = sm70_field(in, 73, 3), words = op == SM70_ATOMG || op == SM70_ATOMG_NOU ? 1
                                                          : type == 5                              ? 2
                                                          : type == 6                              ? 4
                                                                                                   : 1;
                for (u32 w = 0; w < words && in->dst + w < SM70_RZ; w++)
                    rkey[in->dst + w] = 0, rsrc[in->dst + w] = (u16)op;
            }
            continue;
        }
        u32 key = 0;
        int found = 0;
        if (op == SM70_ULDC || (op == SM70_LDC && sm70_field(in, 24, 8) == SM70_RZ))
            key = (sm70_field(in, 54, 5) << 16 | sm70_field(in, 38, 16)) + 1, found = 1;
        else if (op == SM70_MOV || op == SM70_UMOV || op == SM70_R2UR) {
            const Sm70Src* b = &in->b;
            if (op == SM70_R2UR)
                key = rkey[sm70_field(in, 24, 8) & 255];
            else if (b->kind == SM70_SRC_CBUF && !b->bindless)
                key = (u32)(b->bank << 16 | b->value) + 1;
            else if (b->kind == SM70_SRC_REG && b->reg < SM70_RZ)
                key = rkey[b->reg];
            else if (b->kind == SM70_SRC_UREG && b->reg < SM70_URZ)
                key = ukey[b->reg];
            found = 1;
        } else if ((op == SM70_IADD3 || op == SM70_UIADD3 || op == SM70_IMAD || op == SM70_UIMAD ||
                    op == SM70_IMAD_WIDE || op == SM70_UIMAD_WIDE || op == SM70_LEA || op == SM70_ULEA) &&
                   !sm70_field(in, 74, 1) && !((op == SM70_LEA || op == SM70_ULEA) && sm70_field(in, 80, 1))) {
            /* one source with a base: the result is that base at an offset. A constant buffer operand is a base
               itself, as in ptxas's LEA R0, P0, R2, c[0x0][0x60], 0x2 */
            const Sm70Src* s3[3] = {&in->a, &in->b, &in->c};
            u32 count = 0, first = 0, last = 3;
            if (op == SM70_IMAD || op == SM70_UIMAD || op == SM70_IMAD_WIDE || op == SM70_UIMAD_WIDE)
                first = 2; /* a product is an offset: only the sum's c can be the base */
            else if (op == SM70_LEA || op == SM70_ULEA)
                first = 1, last = 2; /* the shifted a is an offset: only b */
            for (u32 k = first; k < last; k++) {
                const Sm70Src* x = s3[k];
                u32 v = x->kind == SM70_SRC_REG && x->reg < SM70_RZ     ? rkey[x->reg]
                        : x->kind == SM70_SRC_UREG && x->reg < SM70_URZ ? ukey[x->reg]
                        : x->kind == SM70_SRC_CBUF && !x->bindless      ? (u32)(x->bank << 16 | x->value) + 1
                                                                        : 0;
                if (v) key = v, count++;
            }
            if (count != 1) key = 0;
            found = 1;
        }
        /* the destination: the key, or none for any other write (the high word of a pair takes none either) */
        u32 d = uniform || op == SM70_ULDC || op == SM70_UMOV || op == SM70_R2UR ? sm70_field(in, 16, 6) : in->dst;
        int to_uniform = uniform || op == SM70_ULDC || op == SM70_R2UR || op == SM70_S2UR || op == SM70_ULDC_IDX;
        int wide = op == SM70_ULDC || op == SM70_LDC || op == SM70_IMAD_WIDE || op == SM70_UIMAD_WIDE;
        if (!found) key = 0;
        if (to_uniform) {
            if (d < SM70_URZ) ukey[d] = key, usrc[d] = (u16)op;
            if (wide && d + 1 < SM70_URZ) ukey[d + 1] = 0;
        } else if ((sm70_ops[op].kind == SM70_K_ALU || op == SM70_LDC || op == SM70_S2R || op == SM70_CS2R) &&
                   op != SM70_FSETP && op != SM70_ISETP && op != SM70_DSETP && op != SM70_HSETP2 && op != SM70_R2P) {
            if (d < SM70_RZ) rkey[d] = key, rsrc[d] = (u16)op;
            if (wide && d + 1 < SM70_RZ) rkey[d + 1] = 0;
        }
    }
    return 1;
}
static u32 shared_var(Ctx* c) {
    if (!c->shared) {
        u32 bytes = c->si->shared_size > 0 ? (u32)c->si->shared_size : 16384u;
        c->shared_words = (bytes + 3) / 4;
        u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, c->shared_words);
        c->shared = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
        c->shared_ptr = spv_emit_type_workgroup_pointer(&c->e, c->t_u32);
        add_interface(c, c->shared);
    }
    return c->shared;
}
/* a word of global (buffer) or shared (buffer 0) memory at the byte address */
static u32 mem_ptr(Ctx* c, u32 buffer, u32 address, u32 word) {
    u32 index =
        spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, address, cu(c, 2)), cu(c, word));
    if (buffer) return spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, buffer, c->zero, index);
    u32 last = cu(c, c->shared_words - 1);
    index = spv_emit_select(&c->e, c->t_u32, spv_emit_u_less(&c->e, c->t_bool, last, index), last, index);
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, c->shared_ptr, shared_var(c), index);
}
/* a load of mem_type (NAK's MemType) into registers dst on */
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst) {
    u32 words = type == 5 ? 2 : type == 6 ? 4 : 1;
    for (u32 k = 0; k < words; k++) {
        u32 ptr = mem_ptr(c, buffer, address, k);
        u32 v = spv_emit_load(&c->e, c->t_u32, ptr);
        if (type < 4) {
            /* a byte or a halfword of the word, sign or zero extended */
            u32 shift = spv_emit_shift_left_u32(
                &c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, address, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
            u32 bits = cu(c, type < 2 ? 8 : 16);
            v = type & 1 ? to_u(c, spv_emit_bitfield_sextract(&c->e, c->t_i32, spv_emit_bitcast(&c->e, c->t_i32, v),
                                                              shift, bits))
                         : spv_emit_bitfield_uextract(&c->e, c->t_u32, v, shift, bits);
        }
        if (uniform_dst)
            uwr(c, dst + k, v);
        else
            wr(c, dst + k, v);
        if (dst >= SM70_RZ) break;
    }
}
static void mem_store(Ctx* c, u32 buffer, u32 address, u32 type, u32 data) {
    if (type < 4) {
        /* a byte or halfword: its bits in the word cleared, then set, by two atomics, so that the other bytes of
           the word, which other invocations can store, stay */
        u32 width = type < 2 ? 8 : 16;
        u32 shift = spv_emit_shift_left_u32(
            &c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, address, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
        u32 mask = spv_emit_shift_left_u32(&c->e, c->t_u32, cu(c, (1u << width) - 1), shift);
        u32 bits = spv_emit_bitwise_and_u32(&c->e, c->t_u32,
                                            spv_emit_shift_left_u32(&c->e, c->t_u32, rd(c, data), shift), mask);
        u32 ptr = mem_ptr(c, buffer, address, 0);
        if (buffer) {
            spv_emit_atomic_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_AND, ptr, spv_emit_not_u32(&c->e, c->t_u32, mask));
            spv_emit_atomic_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_OR, ptr, bits);
        } else {
            spv_emit_atomic_shared_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_AND, ptr,
                                       spv_emit_not_u32(&c->e, c->t_u32, mask));
            spv_emit_atomic_shared_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_OR, ptr, bits);
        }
        return;
    }
    u32 words = type == 5 ? 2 : type == 6 ? 4 : 1;
    for (u32 k = 0; k < words; k++)
        spv_emit_store(&c->e, mem_ptr(c, buffer, address, k), rd(c, data >= SM70_RZ ? data : data + k));
}

/* ---- textures and surfaces ---- */

/* the texture slot of a handle at byte offset off of a bank */
static int tex_slot(Ctx* c, u32 bank, u32 off, u32 type, u32 array, u32 shadow) {
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ntex; i++) {
        const ShTex* t = &si->tex[i];
        if (t->bl_cbuf == bank && t->cbuf_off == off / 4 && t->type == type && t->array == array && t->shadow == shadow)
            return i;
    }
    if (si->ntex >= SH_MAXTEX) {
        fail(c, "more than %d textures", SH_MAXTEX);
        return -1;
    }
    int i = si->ntex++;
    ShTex t = {0};
    t.cbuf_off = off / 4;
    t.type = (u8)type;
    t.array = (u8)array;
    t.shadow = (u8)shadow;
    t.bindless = 1;
    t.bl_cbuf = (u8)bank;
    si->tex[i] = t;
    /* a 1D texture is bound as a 2D one (shader.h) */
    c->tex[i] = spv_emit_sampler_image(&c->e, c->t_f32, (u32)shader_vk_set(c->stage), SNX_VKB_TEX + (u32)i,
                                       type == 0 ? 1 : type, (int)array, (int)shadow, 0, &c->tex_sampled[i]);
    add_interface(c, c->tex[i]);
    return i;
}
/* NAK's TexDim (bits 61-63): ShTex's type (0 1D, 1 2D, 2 3D, 3 cube), array, and the coordinates */
static int tex_dim(u32 dim, u32* type, u32* array, u32* coords) {
    static const u8 types[8] = {0, 1, 2, 3, 0, 1, 0, 3}, arrays[8] = {0, 0, 0, 0, 1, 1, 0, 1},
                    counts[8] = {1, 2, 3, 3, 1, 2, 0, 3};
    if (dim == 6) return 0;
    *type = types[dim];
    *array = arrays[dim];
    *coords = counts[dim];
    return 1;
}
/* The register of each value of a texture instruction's operand list: list0 (the array layer, the coordinates) from
   register a, list1 (the LOD or bias, the offset, the depth reference) from b. The scalar form (.SCR, bit 60) puts
   the whole list in a and b, one or two values each, as NAK's try_scalarize_tex does. Returns 0 when it can't */
static int tex_operands(const Sm70Insn* in, u32 n0, u32 n1, u32* regs) {
    u32 a = sm70_field(in, 24, 8), b = sm70_field(in, 32, 8), n = n0 + n1;
    if (sm70_field(in, 60, 1)) {
        if (n > 4) return 0;
        u32 first = n <= 2 ? 1 : 2;
        for (u32 k = 0; k < n; k++)
            regs[k] = k < first ? (a >= SM70_RZ ? a : a + k) : (b >= SM70_RZ ? b : b + k - first);
        return 1;
    }
    for (u32 k = 0; k < n0; k++) regs[k] = a >= SM70_RZ ? a : a + k;
    for (u32 k = 0; k < n1; k++) regs[n0 + k] = b >= SM70_RZ ? b : b + k;
    return 1;
}
/* the components of a vec4 result that the channel mask selects, into dst and dst + 1, then dst1 and dst1 + 1 (or on
   from dst when dst1 is RZ) */
static void tex_results(Ctx* c, const Sm70Insn* in, u32 value, u32 scalar) {
    u32 mask = sm70_field(in, 72, 4), d0 = in->dst, d1 = sm70_field(in, 64, 8), written = 0;
    for (u32 k = 0; k < 4; k++) {
        if (!(mask & (1u << k))) continue;
        u32 v = spv_emit_vec4_extract(&c->e, scalar, value, k);
        if (scalar != c->t_u32) v = to_u(c, v);
        u32 r = written < 2 || d1 >= SM70_RZ ? (d0 >= SM70_RZ ? d0 : d0 + written) : d1 + written - 2;
        wr(c, r, v);
        written++;
    }
}
static u32 coord_vector(Ctx* c, const u32* f, u32 count) {
    if (count == 1) return f[0];
    if (count == 2) return spv_emit_vec2_construct(&c->e, c->t_v2f, f[0], f[1]);
    if (count == 3) return spv_emit_vec3_construct(&c->e, c->t_v3f, f[0], f[1], f[2]);
    return spv_emit_vec4_construct(&c->e, c->t_v4f, f);
}
static void texture(Ctx* c, const Sm70Insn* in) {
    int op = in->op;
    u32 type, array, coords;
    if (!tex_dim(sm70_field(in, 61, 3), &type, &array, &coords)) {
        fail(c, "%s dimension 6", sm70_ops[op].name);
        return;
    }
    u32 shadow = op == SM70_TMML ? 0 : sm70_field(in, 78, 1);
    u32 lod_mode = op == SM70_TEX ? sm70_field(in, 87, 3) : 0;
    u32 offset = op == SM70_TEX ? sm70_field(in, 76, 1) : op == SM70_TLD4 ? sm70_field(in, 76, 2) : 0;
    if (offset) {
        fail(c, "%s with a texel offset", sm70_ops[op].name);
        return;
    }
    if (lod_mode > 3) {
        fail(c, "TEX LOD mode %u", lod_mode);
        return;
    }
    if (op == SM70_TMML && c->stage != ST_FS) {
        fail(c, "TMML outside a fragment program");
        return;
    }
    u32 handle = sm70_field(in, 40, 19);
    int slot = tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, shadow);
    if (slot < 0) return;
    u32 n0 = array + coords, n1 = (lod_mode == 2 || lod_mode == 3) + shadow, regs[8];
    if (!tex_operands(in, n0, n1, regs)) {
        fail(c, "%s with %u operands in the scalar form", sm70_ops[op].name, n0 + n1);
        return;
    }
    /* the coordinates, a 1D texture's as a 2D one's, then the layer, as SPIR-V orders them */
    u32 f[4], count = 0;
    for (u32 k = 0; k < coords; k++) f[count++] = to_f(c, rd(c, regs[array + k]));
    if (type == 0) f[count++] = cf(c, 0.0f);
    if (array)
        f[count++] = spv_emit_convert_u_to_f(&c->e, c->t_f32,
                                             spv_emit_bitwise_and_u32(&c->e, c->t_u32, rd(c, regs[0]), cu(c, 0xffff)));
    u32 coord = coord_vector(c, f, count);
    u32 next = n0, level = 0, dref = 0;
    if (lod_mode == 2 || lod_mode == 3) level = to_f(c, rd(c, regs[next++]));
    if (shadow) dref = to_f(c, rd(c, regs[next++]));
    u32 result;
    if (op == SM70_TLD4) {
        result = spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord,
                                          cu(c, sm70_field(in, 87, 2)), dref, 0);
    } else if (op == SM70_TMML) {
        u32 lod = spv_emit_image_query_lod(&c->e, c->t_v2f, c->tex_sampled[slot], c->tex[slot], coord);
        u32 v[4] = {0};
        for (u32 k = 0; k < 2; k++)
            v[k] = to_f(c, spv_emit_convert_f_to_s(&c->e, c->t_i32,
                                                   spv_emit_fmul(&c->e, c->t_f32,
                                                                 spv_emit_vec4_extract(&c->e, c->t_f32, lod, 1 - k),
                                                                 cf(c, 256.0f))));
        v[2] = v[3] = cf(c, 0.0f);
        spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
        result = spv_emit_vec4_construct(&c->e, c->t_v4f, v);
    } else {
        /* implicit LOD only in fragment programs: elsewhere the base level */
        u32 mode = lod_mode == 3 ? 1 : lod_mode == 2 ? 2 : lod_mode == 1 ? 1 : 0;
        if (lod_mode == 1) level = cf(c, 0.0f);
        if (mode != 1 && c->stage != ST_FS) {
            mode = 1;
            level = cf(c, 0.0f);
        }
        u32 rtype = shadow ? c->t_f32 : c->t_v4f;
        result =
            spv_emit_sample_image(&c->e, rtype, c->tex_sampled[slot], c->tex[slot], coord, dref, mode, level, 0, 0);
        if (shadow) {
            u32 v[4] = {result, cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
            result = spv_emit_vec4_construct(&c->e, c->t_v4f, v);
        }
    }
    tex_results(c, in, result, c->t_f32);
}
/* TLD: a texel at integer coordinates and an explicit level (bits 87-89, NAK's TexLodMode: 1 the base level, 3 a
   level register). Its operands: the layer and the coordinates, then the level (nak_nir_lower_tex.c) */
static void texel_fetch(Ctx* c, const Sm70Insn* in) {
    u32 type, array, coords, lod_mode = sm70_field(in, 87, 3);
    if (!tex_dim(sm70_field(in, 61, 3), &type, &array, &coords) || type == 3) {
        fail(c, "TLD dimension %u", sm70_field(in, 61, 3));
        return;
    }
    if (sm70_field(in, 76, 1) || sm70_field(in, 78, 1) || (lod_mode != 1 && lod_mode != 3)) {
        fail(c, "TLD with an offset, of a multisample texture or LOD mode %u", lod_mode);
        return;
    }
    u32 handle = sm70_field(in, 40, 19);
    int slot = tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, 0);
    if (slot < 0) return;
    u32 n0 = array + coords, regs[8];
    if (!tex_operands(in, n0, lod_mode == 3, regs)) {
        fail(c, "TLD with %u operands in the scalar form", n0 + (lod_mode == 3));
        return;
    }
    /* the coordinates, a 1D texture's as a 2D one's, then the layer */
    u32 v[4], count = 0;
    for (u32 k = 0; k < coords; k++) v[count++] = rd(c, regs[array + k]);
    if (type == 0) v[count++] = c->zero;
    if (array) v[count++] = spv_emit_bitwise_and_u32(&c->e, c->t_u32, rd(c, regs[0]), cu(c, 0xffff));
    u32 ivec = spv_emit_type_vector(&c->e, c->t_i32, count);
    u32 uvec = spv_emit_type_vector(&c->e, c->t_u32, count);
    u32 coord = spv_emit_bitcast(&c->e, ivec,
                                 count == 2 ? spv_emit_vec2_construct(&c->e, uvec, v[0], v[1])
                                            : spv_emit_vec3_construct(&c->e, uvec, v[0], v[1], v[2]));
    u32 level =
        lod_mode == 3 ? spv_emit_bitcast(&c->e, c->t_i32, rd(c, regs[n0])) : spv_emit_bitcast(&c->e, c->t_i32, c->zero);
    u32 texel = spv_emit_image_fetch(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, level);
    tex_results(c, in, texel, c->t_f32);
}
/* TXQ of the dimensions (bits 62-63 = 0): width, height, depth or layers, and the levels, at the level in register
   a. TXQ names no dimension: the slot of a texture instruction with the same handle, else a 2D texture */
static void texture_query(Ctx* c, const Sm70Insn* in) {
    if (sm70_field(in, 62, 2)) {
        fail(c, "TXQ query %u", sm70_field(in, 62, 2));
        return;
    }
    u32 handle = sm70_field(in, 40, 19), bank = (handle >> 14) & 31, off = handle & 0x3fff;
    int slot = -1;
    for (int i = 0; i < c->si->ntex && slot < 0; i++)
        if (c->si->tex[i].bl_cbuf == bank && c->si->tex[i].cbuf_off == off) slot = i;
    if (slot < 0) slot = tex_slot(c, bank, off * 4, 1, 0, 0);
    if (slot < 0) return;
    const ShTex* t = &c->si->tex[slot];
    u32 dims = (t->type == 2 ? 3 : 2) + t->array; /* a 1D texture is a 2D one, a cube's size is its face's */
    if (t->type == 3) dims = 2 + t->array;
    spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
    u32 size =
        spv_emit_image_query_size_lod(&c->e, spv_emit_type_vector(&c->e, c->t_u32, dims), c->tex_sampled[slot],
                                      c->tex[slot], spv_emit_bitcast(&c->e, c->t_i32, rd(c, sm70_field(in, 24, 8))));
    u32 v[4];
    for (u32 k = 0; k < 3; k++) v[k] = k < dims ? SPV(&c->e, SPV_OP_COMPOSITE_EXTRACT, c->t_u32, size, k) : cu(c, 1);
    if (t->type == 0) v[1] = cu(c, 1);
    v[3] = spv_emit_image_query_levels(&c->e, c->t_u32, c->tex_sampled[slot], c->tex[slot]);
    tex_results(c, in, spv_emit_vec4_construct(&c->e, c->t_v4u, v), c->t_u32);
}
/* the sampled type of a storage image of a kind: float (0), unsigned (1, 2), signed (3) */
static u32 image_scalar(Ctx* c, u32 kind) { return kind == 0 ? c->t_f32 : kind == 3 ? c->t_i32 : c->t_u32; }
/* NAK's ImageDim (bits 61-63): the SPIR-V dimension, arrayed, and the integer coordinates */
static int image_dim(u32 dim, u32* sdim, u32* array, u32* coords, u32* type) {
    /* ShaderInfo.img[].type is the code itself (0 1D, 1 1D buffer, 2 1D array, 3 2D, 4 2D array, 5 3D), which the
       renderer reads as Maxwell's SUST dimension */
    *type = dim;
    switch (dim) {
        case 0: *sdim = 0, *array = 0, *coords = 1; return 1;
        case 2: *sdim = 0, *array = 1, *coords = 2; return 1;
        case 3: *sdim = 1, *array = 0, *coords = 2; return 1;
        case 4: *sdim = 1, *array = 1, *coords = 3; return 1;
        case 5: *sdim = 2, *array = 0, *coords = 3; return 1;
        default: return 0;
    }
}
static void surface(Ctx* c, const Sm70Insn* in) {
    int store = in->op == SM70_SUST_P, atomic = in->op == SM70_SURED;
    u32 sdim, array, coords, type;
    if (!image_dim(sm70_field(in, 61, 3), &sdim, &array, &coords, &type)) {
        fail(c, "%s dimension %u", sm70_ops[in->op].name, sm70_field(in, 61, 3));
        return;
    }
    u32 handle = sm70_field(in, 40, 19), bank = (handle >> 14) & 31, off = handle & 0x3fff;
    ShaderInfo* si = c->si;
    int slot = -1;
    for (int i = 0; i < si->nimg; i++)
        if (si->img[i].bl_cbuf == bank && si->img[i].cbuf_off == off && si->img[i].type == type &&
            (si->img[i].kind == 1) == atomic)
            slot = i;
    if (slot < 0) {
        if (si->nimg >= 8) {
            fail(c, "more than 8 storage images");
            return;
        }
        slot = si->nimg++;
        /* the kind, as spirv_translate.c's rule for a bindless image: an R32ui image for an atomic (1); else the
           renderer's key, bit 8 + slot of rt_int_mask for a format of the other class, gives a float image (0) or
           an unsigned one (2), and of rt_sint_mask a signed one (3) */
        u32 kind = atomic ? 1 : si->rt_int_mask & (1u << (8 + slot)) ? 0 : 2;
        if (!atomic && (si->rt_sint_mask & (1u << (8 + slot)))) kind = 3;
        si->img[slot].cbuf_off = off;
        si->img[slot].type = (u8)type;
        si->img[slot].bindless = 1;
        si->img[slot].bl_cbuf = (u8)bank;
        si->img[slot].kind = (u8)kind;
        if (kind != 1) spv_emit_capability(&c->e, 55); /* StorageImageReadWithoutFormat */
        c->img[slot] =
            spv_emit_storage_image(&c->e, image_scalar(c, kind), (u32)shader_vk_set(c->stage), SNX_VKB_IMG + (u32)slot,
                                   sdim == 0 ? 1 : sdim, (int)array, kind == 1 ? 33 : 0, &c->img_type[slot]);
        add_interface(c, c->img[slot]);
    }
    /* integer coordinates, a 1D image's as a 2D one's */
    u32 a = sm70_field(in, 24, 8), v[4], count = 0;
    for (u32 k = 0; k < coords; k++) {
        if (sdim == 0 && k == 1) v[count++] = c->zero;
        v[count++] = rd(c, a >= SM70_RZ ? a : a + k);
    }
    if (sdim == 0 && coords == 1) v[count++] = c->zero;
    u32 ivec = count == 2 ? spv_emit_type_vec2(&c->e, c->t_i32) : spv_emit_type_vec3(&c->e, c->t_i32);
    u32 uvec = count == 2 ? c->t_v2u : c->t_v3u;
    u32 coord = spv_emit_bitcast(&c->e, ivec,
                                 count == 2 ? spv_emit_vec2_construct(&c->e, uvec, v[0], v[1])
                                            : spv_emit_vec3_construct(&c->e, uvec, v[0], v[1], v[2]));
    if (atomic) {
        /* SURED: NAK's set_atom_op at 87-89 (0 add, 1 min, 2 max, 5 and, 6 or, 7 xor) and atom type at 73-75 (0
           u32, 1 s32), the data at 32 */
        static const u16 ops[2][8] = {{SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_U_MIN, SPV_OP_ATOMIC_U_MAX, 0, 0,
                                       SPV_OP_ATOMIC_AND, SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR},
                                      {SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_S_MIN, SPV_OP_ATOMIC_S_MAX, 0, 0,
                                       SPV_OP_ATOMIC_AND, SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR}};
        u32 how = sm70_field(in, 87, 3), atype = sm70_field(in, 73, 3);
        if (atype > 1 || !ops[atype][how]) {
            fail(c, "SURED of type %u, operation %u", atype, how);
            return;
        }
        u32 ptr = spv_emit_image_texel_pointer(&c->e, c->t_u32, c->img[slot], coord, c->zero);
        spv_emit_atomic_u32(&c->e, c->t_u32, ops[atype][how], ptr, rd(c, sm70_field(in, 32, 8)));
        return;
    }
    /* the texel in the image's own type: the register words as floats (kind 0) or integers (2, 3); a component
       that the mask leaves out is 0, the alpha 1 */
    u32 kind = si->img[slot].kind, scalar = image_scalar(c, kind);
    u32 vec4 = spv_emit_type_vec4(&c->e, scalar);
    u32 mask = sm70_field(in, 72, 4);
    if (store) {
        u32 data = sm70_field(in, 32, 8), f[4], next = 0;
        for (u32 k = 0; k < 4; k++) {
            if (mask & (1u << k))
                f[k] = spv_emit_bitcast(&c->e, scalar, rd(c, data >= SM70_RZ ? data : data + next++));
            else
                f[k] = kind == 0 ? cf(c, k == 3 ? 1.0f : 0.0f) : spv_emit_bitcast(&c->e, scalar, cu(c, k == 3));
        }
        spv_emit_image_write(&c->e, c->img_type[slot], c->img[slot], coord, spv_emit_vec4_construct(&c->e, vec4, f));
    } else {
        u32 texel = spv_emit_image_read(&c->e, vec4, c->img_type[slot], c->img[slot], coord);
        u32 d = in->dst, written = 0;
        for (u32 k = 0; k < 4; k++)
            if (mask & (1u << k)) {
                wr(c, d >= SM70_RZ ? d : d + written, to_u(c, spv_emit_vec4_extract(&c->e, scalar, texel, k)));
                written++;
            }
    }
}

static u32 system_value(Ctx* c, u32 sr);
/* the ballot of a predicate over the invocation's 32 lanes, on a subgroup of 32 or 64 */
static u32 ballot32(Ctx* c, u32 pred) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    u32 all = spv_emit_subgroup_ballot(&c->e, c->t_v4u, c->t_u32, pred);
    u32 half = spv_emit_shift_right_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, 5));
    return SPV(&c->e, 77 /* OpVectorExtractDynamic */, c->t_u32, all, half);
}

/* ---- one instruction ---- */

/* the fragment program's outputs from the registers when it ends: the colours of the render targets the SPH's omap
   enables, four registers a target, then the depth (Maxwell's layout) */
static void epilogue(Ctx* c) {
    if (c->stage == ST_VS && c->position && !(c->si->clip_enable & SNX_SH_NOT_LAST)) vertex_depth_fix(c);
    if (c->stage != ST_FS) return;
    u32 target = c->sph[18], reg = 0;
    for (u32 rt = 0; rt < 8; rt++) {
        if (!((target >> (rt * 4)) & 15)) continue;
        if (!c->color[rt]) {
            c->color[rt] = spv_emit_output_vec4(&c->e, c->t_v4f, rt);
            add_interface(c, c->color[rt]);
        }
        u32 v[4];
        for (u32 k = 0; k < 4; k++) v[k] = to_f(c, rd(c, reg + k));
        spv_emit_store(&c->e, c->color[rt], spv_emit_vec4_construct(&c->e, c->t_v4f, v));
        reg += 4;
    }
    if (c->sph[19] & 1u) reg++; /* the sample mask, which a single-sampled target ignores */
    if (c->sph[19] & 2u) {
        if (!c->depth) {
            c->depth = spv_emit_frag_depth_output(&c->e, c->t_f32);
            add_interface(c, c->depth);
        }
        spv_emit_store(&c->e, c->depth, to_f(c, rd(c, reg)));
    }
}
/* an S2R system register */
static u32 system_value(Ctx* c, u32 sr) {
    switch (sr) {
        case 0x00: /* LANEID */
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
            return builtin_u32(c, &c->lane_id, 41);
        case 0x21:
        case 0x22:
        case 0x23: return builtin_vec3(c, &c->local_id, 27, sr - 0x21);
        case 0x25:
        case 0x26:
        case 0x27: return builtin_vec3(c, &c->group_id, 26, sr - 0x25);
        case 0x38: /* EQMASK, LTMASK, LEMASK, GTMASK, GEMASK: of the lane in its 32 */
        case 0x39:
        case 0x3a:
        case 0x3b:
        case 0x3c: {
            u32 lane = spv_emit_bitwise_and_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, 31));
            u32 eq = spv_emit_shift_left_u32(&c->e, c->t_u32, cu(c, 1), lane);
            u32 lt = spv_emit_isub_u32(&c->e, c->t_u32, eq, cu(c, 1));
            u32 le = spv_emit_bitwise_or_u32(&c->e, c->t_u32, lt, eq);
            return sr == 0x38   ? eq
                   : sr == 0x39 ? lt
                   : sr == 0x3a ? le
                   : sr == 0x3b ? spv_emit_not_u32(&c->e, c->t_u32, le)
                                : spv_emit_not_u32(&c->e, c->t_u32, lt);
        }
        default: fail(c, "S2R system register 0x%x", sr); return c->zero;
    }
}
static int stage_ok(Ctx* c, const Sm70Insn* in) {
    int op = in->op;
    int compute =
        op == SM70_S2R || op == SM70_S2UR || op == SM70_LDS || op == SM70_STS || op == SM70_STS_NOU || op == SM70_BAR;
    int fragment = op == SM70_IPA || op == SM70_KILL || op == SM70_TMML;
    if (compute && (op == SM70_S2R || op == SM70_S2UR)) compute = sm70_field(in, 72, 8) != 0;
    if (compute && c->stage != ST_CS) return 0;
    if (fragment && c->stage != ST_FS) return 0;
    return 1;
}

/* IADD3 and UIADD3: a + b + c, with the carries out in the predicates p and q and, for .X, two carries in */
static void emit_insn_iadd3(Ctx* c, const Sm70Insn* in, int uniform, u32 p, u32 q, int x) {
    if (p >= SM70_PT && q >= SM70_PT && !x) {
        u32 v = spv_emit_iadd_u32(&c->e, c->t_u32, src_int(c, &in->a), src_int(c, &in->b));
        dwr(c, in, spv_emit_iadd_u32(&c->e, c->t_u32, v, src_int(c, &in->c)));
        return;
    }
    /* the sum in 64 bits: its carries, and .X's two carries in */
    u32 sum = op2(c, SPV_OP_I_ADD, u64_type(c), term64(c, &in->a), term64(c, &in->b));
    sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, term64(c, &in->c));
    if (x) {
        sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, carry_in(c, 87, in, uniform));
        sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, carry_in(c, 77, in, uniform));
    }
    u32 high = hi32(c, sum);
    dwr(c, in, lo32(c, sum));
    pwr(c, p, spv_emit_u_less(&c->e, c->t_bool, c->zero, high), uniform);
    pwr(c, q, spv_emit_u_less(&c->e, c->t_bool, cu(c, 1), high), uniform);
}

static void emit_insn(Ctx* c, const Sm70Insn* in) {
    int op = in->op, uniform = sm70_ops[op].kind == SM70_K_UALU;
    switch (op) {
        case SM70_NOP:
        case SM70_DEPBAR:
        case SM70_BSSY:
        case SM70_BSYNC:
        case SM70_WARPSYNC: return;
        case SM70_MOV: wr(c, in->dst, src_bits(c, &in->b)); return;
        case SM70_FADD:
        case SM70_FMUL:
        case SM70_FFMA: {
            /* bit 77 saturate, 78-79 the rounding mode, 80 FTZ, and FMUL's 84-86 the scale 2^(field - 4), which
               nvdisasm names .D4 (2), .D2 (3) and none (4) */
            int on = (int)sm70_field(in, 80, 1);
            u32 mode = sm70_field(in, 78, 2), scale = sm70_field(in, 84, 3), v;
            note_ftz(c, on);
            u32 x = ftz(c, src_f(c, &in->a), on);
            if (op == SM70_FADD)
                v = float_add(c, mode, x, ftz(c, src_f(c, second(in)), on));
            else if (op == SM70_FMUL) {
                if (!scale) {
                    fail(c, "FMUL with scale field 0");
                    return;
                }
                v = float_mul(c, mode, scale, x, ftz(c, src_f(c, &in->b), on));
            } else
                v = float_fma(c, mode, x, ftz(c, src_f(c, &in->b), on), ftz(c, src_f(c, &in->c), on));
            dwr(c, in, to_u(c, saturate(c, ftz(c, v, on), (int)sm70_field(in, 77, 1))));
            return;
        }
        case SM70_FMNMX: {
            u32 min = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), 0);
            int on = (int)sm70_field(in, 80, 1);
            u32 x = ftz(c, src_f(c, &in->a), on), y = ftz(c, src_f(c, &in->b), on);
            u32 lo = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN, x, y);
            u32 hi = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, y);
            dwr(c, in, to_u(c, spv_emit_select(&c->e, c->t_f32, min, lo, hi)));
            return;
        }
        case SM70_FSET: {
            int on = (int)sm70_field(in, 80, 1);
            u32 cmp = spv_emit_fcompare(&c->e, c->t_bool, sm70_field(in, 76, 4), ftz(c, src_f(c, &in->a), on),
                                        ftz(c, src_f(c, &in->b), on));
            u32 acc = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), 0);
            cmp = combine(c, sm70_field(in, 74, 2), cmp, acc);
            dwr(c, in, to_u(c, spv_emit_select(&c->e, c->t_f32, cmp, cf(c, 1.0f), cf(c, 0.0f))));
            return;
        }
        case SM70_FSETP:
        case SM70_UISETP:
        case SM70_ISETP: {
            u32 cmp;
            if (op == SM70_FSETP) {
                int on = (int)sm70_field(in, 80, 1);
                cmp = spv_emit_fcompare(&c->e, c->t_bool, sm70_field(in, 76, 4), ftz(c, src_f(c, &in->a), on),
                                        ftz(c, src_f(c, &in->b), on));
            } else {
                cmp = int_compare(c, sm70_field(in, 76, 3), (int)sm70_field(in, 73, 1), src_int(c, &in->a),
                                  src_int(c, &in->b));
                if (sm70_field(in, 72, 1)) { /* .EX: equal high words take the low words' comparison */
                    u32 low = prd(c, sm70_field(in, 68, 3), sm70_field(in, 71, 1), uniform);
                    u32 equal = spv_emit_i_equal(&c->e, c->t_bool, src_int(c, &in->a), src_int(c, &in->b));
                    u32 cmp_op = sm70_field(in, 76, 3);
                    if (cmp_op != 0 && cmp_op != 7) cmp = spv_emit_select(&c->e, c->t_bool, equal, low, cmp);
                }
            }
            u32 acc = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), uniform);
            u32 set_op = sm70_field(in, 74, 2);
            pwr(c, sm70_field(in, 81, 3), combine(c, set_op, cmp, acc), uniform);
            pwr(c, sm70_field(in, 84, 3), combine(c, set_op, spv_emit_bool_not(&c->e, c->t_bool, cmp), acc), uniform);
            return;
        }
        case SM70_FSEL:
        case SM70_USEL:
        case SM70_SEL: {
            u32 p = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), uniform);
            u32 x = op == SM70_FSEL ? to_u(c, src_f(c, &in->a)) : src_bits(c, &in->a);
            u32 y = op == SM70_FSEL ? to_u(c, src_f(c, &in->b)) : src_bits(c, &in->b);
            dwr(c, in, spv_emit_select(&c->e, c->t_u32, p, x, y));
            return;
        }
        case SM70_MUFU: {
            u32 x = src_f(c, &in->b), v = 0, fn = sm70_field(in, 74, 6);
            if (sm70_field(in, 73, 1)) {
                fail(c, "MUFU.F16");
                return;
            }
            switch (fn) {
                case 0:
                case 1: /* the argument in turns, as the hardware takes it */
                    v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, fn ? SPV_GLSL_SIN : SPV_GLSL_COS,
                                            spv_emit_fmul(&c->e, c->t_f32, x, cf(c, 6.28318530717958647692f)));
                    break;
                case 2: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_EXP2, x); break;
                case 3: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_LOG2, x); break;
                case 4: v = spv_emit_fdiv(&c->e, c->t_f32, cf(c, 1.0f), x); break;
                case 5: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_INVERSE_SQRT, x); break;
                case 8: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_SQRT, x); break;
                case 9: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, (SpvGlsl450Op)21 /* Tanh */, x); break;
                default: fail(c, "MUFU function %u", fn); return;
            }
            dwr(c, in, to_u(c, v));
            return;
        }
        case SM70_F2I: {
            u32 dsize = sm70_field(in, 75, 2); /* NAK: log2 of the destination's bytes */
            if (dsize == 3 || sm70_field(in, 84, 2) != 2) {
                fail(c, "F2I of sizes %u, %u", sm70_field(in, 84, 2), dsize);
                return;
            }
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            u32 x = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[sm70_field(in, 78, 2)], src_f(c, &in->b));
            int is_signed = (int)sm70_field(in, 72, 1);
            /* the hardware saturates, and a NaN gives 0 */
            u32 nan = spv_emit_isnan(&c->e, c->t_bool, x), v;
            if (dsize < 2) {
                /* an 8- or 16-bit result saturates to its range, then fills the register sign or zero extended */
                u32 bits = 8u << dsize;
                float lo = is_signed ? -(float)(1u << (bits - 1)) : 0.0f;
                float hi = is_signed ? (float)((1u << (bits - 1)) - 1) : (float)((1u << bits) - 1);
                u32 clamped = spv_emit_glsl_binary(
                    &c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN,
                    spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, lo)), cf(c, hi));
                v = is_signed ? to_u(c, spv_emit_convert_f_to_s(&c->e, c->t_i32, clamped))
                              : spv_emit_convert_f_to_u(&c->e, c->t_u32, clamped);
            } else if (is_signed) {
                u32 high = spv_emit_fcompare(&c->e, c->t_bool, 6, x, cf(c, 2147483648.0f));
                u32 clamped = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, -2147483648.0f));
                v = to_u(c, spv_emit_convert_f_to_s(&c->e, c->t_i32, clamped));
                v = spv_emit_select(&c->e, c->t_u32, high, cu(c, 0x7fffffffu), v);
            } else {
                u32 high = spv_emit_fcompare(&c->e, c->t_bool, 6, x, cf(c, 4294967296.0f));
                u32 clamped = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, 0.0f));
                v = spv_emit_convert_f_to_u(&c->e, c->t_u32, clamped);
                v = spv_emit_select(&c->e, c->t_u32, high, cu(c, 0xffffffffu), v);
            }
            dwr(c, in, spv_emit_select(&c->e, c->t_u32, nan, c->zero, v));
            return;
        }
        case SM70_I2F:
        case SM70_I2FP: {
            u32 src_size = sm70_field(in, 84, 2);
            if (sm70_field(in, 75, 2) != 2 || src_size == 3) {
                fail(c, "%s of sizes %u, %u", sm70_ops[op].name, src_size, sm70_field(in, 75, 2));
                return;
            }
            /* the rounding mode in 78-79: a 32-bit integer is exact in a double. An 8- or 16-bit source (84-85) is
               the byte (.B1 to .B3) or halfword (.H1) that 60-61 select (CuAssembler sm_86) */
            u32 x = src_bits(c, &in->b), mode = sm70_field(in, 78, 2), is_signed = sm70_field(in, 74, 1);
            if (src_size < 2) {
                u32 width = src_size ? 16 : 8, at = cu(c, width * sm70_field(in, 60, 2));
                x = is_signed ? to_u(c, spv_emit_bitfield_sextract(
                                            &c->e, c->t_i32, spv_emit_bitcast(&c->e, c->t_i32, x), at, cu(c, width)))
                              : spv_emit_bitfield_uextract(&c->e, c->t_u32, x, at, cu(c, width));
            }
            u32 type = mode ? f64_type(c) : c->t_f32;
            u32 v = is_signed ? spv_emit_convert_s_to_f(&c->e, type, spv_emit_bitcast(&c->e, c->t_i32, x))
                              : spv_emit_convert_u_to_f(&c->e, type, x);
            if (mode) v = round_f64(c, mode, v);
            dwr(c, in, to_u(c, v));
            return;
        }
        case SM70_FRND: {
            if (sm70_field(in, 75, 2) != 2 || sm70_field(in, 84, 2) != 2) {
                fail(c, "FRND of 64-bit or 16-bit floats");
                return;
            }
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            dwr(c, in,
                to_u(c,
                     spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[sm70_field(in, 78, 2)], src_f(c, &in->b))));
            return;
        }
        case SM70_IADD3:
        case SM70_UIADD3: {
            u32 p = sm70_field(in, 81, 3), q = sm70_field(in, 84, 3);
            int x = (int)sm70_field(in, 74, 1);
            emit_insn_iadd3(c, in, uniform, p, q, x);
            return;
        }
        case SM70_UIMAD:
        case SM70_IMAD: {
            /* .X (bit 74) adds the carry predicate at 87-90, as IADD3.X does */
            u32 v = spv_emit_imul_u32(&c->e, c->t_u32, src_int(c, &in->a), src_int(c, &in->b));
            v = spv_emit_iadd_u32(&c->e, c->t_u32, v, src_int(c, &in->c));
            if (sm70_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_select(&c->e, c->t_u32,
                                                      prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), uniform),
                                                      cu(c, 1), c->zero));
            dwr(c, in, v);
            return;
        }
        case SM70_UIMNMX:
        case SM70_IMNMX: {
            u32 min = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), uniform);
            u32 x = src_bits(c, &in->a), y = src_bits(c, &in->b);
            u32 v;
            if (sm70_field(in, 73, 1)) {
                u32 xi = spv_emit_bitcast(&c->e, c->t_i32, x), yi = spv_emit_bitcast(&c->e, c->t_i32, y);
                v = to_u(c, spv_emit_select(&c->e, c->t_i32, min,
                                            spv_emit_glsl_binary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_MIN, xi, yi),
                                            spv_emit_glsl_binary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_MAX, xi, yi)));
            } else
                v = spv_emit_select(&c->e, c->t_u32, min,
                                    spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, x, y),
                                    spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MAX, x, y));
            dwr(c, in, v);
            return;
        }
        case SM70_FLO:
        case SM70_UFLO: {
            /* NAK's OpFlo: bit 73 signed, 74 the shift amount (31 - the bit) in place of the bit, 63 the source
               inverted. No 1 gives 0xffffffff, as FindUMsb's -1 */
            u32 x = src_bits(c, &in->b);
            if (sm70_field(in, 63, 1)) x = spv_emit_not_u32(&c->e, c->t_u32, x);
            u32 bit = sm70_field(in, 73, 1)
                          ? to_u(c, SPV_EXT(&c->e, c->t_i32, 74 /* FindSMsb */, spv_emit_bitcast(&c->e, c->t_i32, x)))
                          : SPV_EXT(&c->e, c->t_u32, 75 /* FindUMsb */, x);
            if (sm70_field(in, 74, 1))
                bit = spv_emit_select(&c->e, c->t_u32, spv_emit_i_equal(&c->e, c->t_bool, bit, cu(c, 0xffffffffu)), bit,
                                      spv_emit_isub_u32(&c->e, c->t_u32, cu(c, 31), bit));
            dwr(c, in, bit);
            return;
        }
        case SM70_POPC:
        case SM70_UPOPC: {
            u32 x = src_bits(c, &in->b);
            if (sm70_field(in, 63, 1)) x = spv_emit_not_u32(&c->e, c->t_u32, x);
            dwr(c, in, spv_emit_bit_count(&c->e, c->t_u32, x));
            return;
        }
        case SM70_BREV:
        case SM70_UBREV: dwr(c, in, spv_emit_bit_reverse_u32(&c->e, c->t_u32, src_bits(c, &in->b))); return;
        case SM70_R2UR: /* the register is the same in each invocation: its value as it is */
            uwr(c, sm70_field(in, 16, 6), rd(c, sm70_field(in, 24, 8)));
            return;
        case SM70_P2R:
        case SM70_UP2UR: {
            /* P2R Rd, PR, Ra, mask: Ra with the masked bits (at byte 76-77) from P0-P6, PT the bit 7 */
            u32 byte = sm70_field(in, 76, 2), mask = in->b.value, pr = 0;
            if (in->b.kind != SM70_SRC_IMM) {
                fail(c, "%s with a register mask", sm70_ops[op].name);
                return;
            }
            u32 bits = cu(c, 0x80);
            for (u32 i = 0; i < 7; i++)
                if (mask & (1u << i))
                    bits = spv_emit_bitwise_or_u32(
                        &c->e, c->t_u32, bits,
                        spv_emit_select(&c->e, c->t_u32, prd(c, i, 0, uniform), cu(c, 1u << i), c->zero));
            pr = spv_emit_bitwise_and_u32(&c->e, c->t_u32, bits, cu(c, mask & 0xff));
            u32 keep =
                spv_emit_bitwise_and_u32(&c->e, c->t_u32, src_bits(c, &in->a), cu(c, ~((mask & 0xffu) << (8 * byte))));
            dwr(c, in,
                spv_emit_bitwise_or_u32(&c->e, c->t_u32, keep,
                                        spv_emit_shift_left_u32(&c->e, c->t_u32, pr, cu(c, 8 * byte))));
            return;
        }
        case SM70_F2FP: {
            /* F2FP.PACK_AB Rd, Ra, Rb: a in the high half, b in the low one, as PTX's cvt.rn.f16x2.f32; bit 76
               BF16 (CuAssembler sm_86). The halves round to nearest */
            if (sm70_field(in, 78, 1)) {
                fail(c, "F2FP.MERGE_C");
                return;
            }
            u32 hi = src_f(c, &in->a), lo = src_f(c, second(in));
            u32 v;
            if (sm70_field(in, 76, 1))
                v = spv_emit_bitwise_or_u32(
                    &c->e, c->t_u32, spv_emit_shift_left_u32(&c->e, c->t_u32, bf16(c, hi), cu(c, 16)), bf16(c, lo));
            else
                v = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, spv_emit_vec2_construct(&c->e, c->t_v2f, lo, hi));
            dwr(c, in, v);
            return;
        }
        case SM70_IABS:
            dwr(c, in,
                to_u(c, spv_emit_glsl_unary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_ABS,
                                            spv_emit_bitcast(&c->e, c->t_i32, src_bits(c, &in->b)))));
            return;
        case SM70_ULEA:
        case SM70_LEA: {
            u32 shift = sm70_field(in, 75, 5), p = sm70_field(in, 81, 3);
            int high = (int)sm70_field(in, 80, 1), x = (int)sm70_field(in, 74, 1);
            u32 a = src_bits(c, &in->a), shifted;
            if (high) /* the high word of (c:a) << shift */
                shifted = shift ? spv_emit_bitwise_or_u32(
                                      &c->e, c->t_u32,
                                      spv_emit_shift_left_u32(&c->e, c->t_u32, src_bits(c, &in->c), cu(c, shift)),
                                      spv_emit_shift_right_u32(&c->e, c->t_u32, a, cu(c, 32 - shift)))
                                : src_bits(c, &in->c);
            else
                shifted = spv_emit_shift_left_u32(&c->e, c->t_u32, a, cu(c, shift));
            if (sm70_field(in, 72, 1)) shifted = spv_emit_isub_u32(&c->e, c->t_u32, c->zero, shifted);
            if (p >= SM70_PT && !x) {
                dwr(c, in, spv_emit_iadd_u32(&c->e, c->t_u32, shifted, src_int(c, &in->b)));
                return;
            }
            u32 sum = op2(c, SPV_OP_I_ADD, u64_type(c), to64(c, shifted), to64(c, src_int(c, &in->b)));
            if (x) sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, carry_in(c, 87, in, uniform));
            dwr(c, in, lo32(c, sum));
            pwr(c, p, spv_emit_u_less(&c->e, c->t_bool, c->zero, hi32(c, sum)), uniform);
            return;
        }
        case SM70_ULOP3:
        case SM70_LOP3: {
            if (sm70_field(in, 80, 1) || sm70_field(in, 87, 4) != 0xf) {
                fail(c, "LOP3 with a predicate operand");
                return;
            }
            u32 v = lut3(c, sm70_field(in, 72, 8), c->t_u32, src_bits(c, &in->a), src_bits(c, &in->b),
                         src_bits(c, &in->c), cu(c, 0xffffffffu), c->zero);
            dwr(c, in, v);
            pwr(c, sm70_field(in, 81, 3), spv_emit_i_not_equal(&c->e, c->t_bool, v, c->zero), uniform);
            return;
        }
        case SM70_USHF:
        case SM70_SHF: {
            u32 dtype = sm70_field(in, 73, 2), bits = dtype < 2 ? 64 : 32;
            int right = (int)sm70_field(in, 76, 1), wrap = (int)sm70_field(in, 75, 1),
                high = (int)sm70_field(in, 80, 1);
            int is_signed = dtype == 0 || dtype == 2;
            u32 s = src_bits(c, &in->b);
            s = wrap ? spv_emit_bitwise_and_u32(&c->e, c->t_u32, s, cu(c, bits - 1))
                     : spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, s, cu(c, bits));
            u32 value = pair64(c, src_bits(c, &in->a), src_bits(c, &in->c));
            /* a shift by 64 is out of SPIR-V's range: split it in two */
            u32 s64 = to64(c, s), half = c64(c, 0);
            u32 big = spv_emit_u_less(&c->e, c->t_bool, cu(c, 63), s);
            u32 s_part = spv_emit_select(&c->e, u64_type(c), big, c64(c, 63), s64);
            u32 extra = spv_emit_select(&c->e, u64_type(c), big, c64(c, 1), half);
            u32 opcode = !right      ? SPV_OP_SHIFT_LEFT_LOGICAL
                         : is_signed ? SPV_OP_SHIFT_RIGHT_ARITHMETIC
                                     : SPV_OP_SHIFT_RIGHT_LOGICAL;
            u32 shifted = op2(c, opcode, u64_type(c), op2(c, opcode, u64_type(c), value, s_part), extra);
            dwr(c, in, high ? hi32(c, shifted) : lo32(c, shifted));
            return;
        }
        case SM70_R2P: {
            u32 byte = sm70_field(in, 76, 2); /* nvdisasm's .B1 sets bit 76, .B2 bit 77 */
            u32 mask = in->b.kind == SM70_SRC_IMM ? in->b.value : 0;
            if (in->b.kind != SM70_SRC_IMM) {
                fail(c, "R2P with a register mask");
                return;
            }
            u32 x = src_bits(c, &in->a);
            for (u32 i = 0; i < 7; i++)
                if (mask & (1u << i))
                    pwr(c, i,
                        spv_emit_i_not_equal(&c->e, c->t_bool,
                                             spv_emit_bitwise_and_u32(&c->e, c->t_u32, x, cu(c, 1u << (i + 8 * byte))),
                                             c->zero),
                        0);
            return;
        }
        case SM70_UPLOP3:
        case SM70_PLOP3: {
            /* bit 67: the third source is a uniform predicate (NAK) */
            int u = op == SM70_UPLOP3; /* all of UPLOP3's predicates are uniform (NAK) */
            u32 x = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), u);
            u32 y = prd(c, sm70_field(in, 77, 3), sm70_field(in, 80, 1), u);
            u32 z = prd(c, sm70_field(in, 68, 3), sm70_field(in, 71, 1), u || (int)sm70_field(in, 67, 1));
            u32 lut0 = sm70_field(in, 64, 3) | sm70_field(in, 72, 5) << 3, lut1 = sm70_field(in, 16, 8);
            u32 p = sm70_field(in, 81, 3), q = sm70_field(in, 84, 3);
            if (p < SM70_PT) pwr(c, p, lut3(c, lut0, c->t_bool, x, y, z, c->true_v, c->false_v), u);
            if (q < SM70_PT) pwr(c, q, lut3(c, lut1, c->t_bool, x, y, z, c->true_v, c->false_v), u);
            return;
        }
        case SM70_CS2R: {
            if (sm70_field(in, 72, 8) != 0xff) {
                fail(c, "CS2R of system register 0x%x", sm70_field(in, 72, 8));
                return;
            }
            wr(c, in->dst, c->zero);
            if (sm70_field(in, 80, 1) && in->dst < SM70_RZ) wr(c, in->dst + 1, c->zero);
            return;
        }
        case SM70_S2R: wr(c, in->dst, system_value(c, sm70_field(in, 72, 8))); return;
        case SM70_SHFL_II:
        case SM70_SHFL_RI:
        case SM70_SHFL_IR:
        case SM70_SHFL_RR: {
            /* PTX's shfl.sync: c's bits 8-12 split the 32 lanes into segments, its bits 0-4 clamp the lane. Mode
               (58-59) 0 IDX, 1 UP, 2 DOWN, 3 BFLY. The lane is at 53-57 (an immediate) or 32 (a register), c at
               40-52 (an immediate) or 64 (a register, which the translation does not take) */
            if (op == SM70_SHFL_IR || op == SM70_SHFL_RR) {
                fail(c, "SHFL with c in a register");
                return;
            }
            u32 mode = sm70_field(in, 58, 2), cc = sm70_field(in, 40, 13);
            u32 seg = (cc >> 8) & 31, clamp = cc & 31;
            u32 b = op == SM70_SHFL_II ? cu(c, sm70_field(in, 53, 5)) : rd(c, sm70_field(in, 32, 8));
            u32 id = system_value(c, 0), lane = spv_emit_bitwise_and_u32(&c->e, c->t_u32, id, cu(c, 31));
            u32 min_lane = spv_emit_bitwise_and_u32(&c->e, c->t_u32, lane, cu(c, seg));
            u32 max_lane = spv_emit_bitwise_or_u32(&c->e, c->t_u32, min_lane, cu(c, clamp & ~seg & 31));
            u32 j, ok;
            if (mode == 0) {
                j = spv_emit_bitwise_or_u32(&c->e, c->t_u32, min_lane,
                                            spv_emit_bitwise_and_u32(&c->e, c->t_u32, b, cu(c, ~seg & 31)));
                ok = op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, j, max_lane);
            } else if (mode == 1) {
                j = spv_emit_isub_u32(&c->e, c->t_u32, lane, b);
                ok = op2(c, SPV_OP_S_GREATER_THAN_EQUAL, c->t_bool, spv_emit_bitcast(&c->e, c->t_i32, j),
                         spv_emit_bitcast(&c->e, c->t_i32, min_lane));
            } else {
                j = mode == 2 ? spv_emit_iadd_u32(&c->e, c->t_u32, lane, b)
                              : spv_emit_bitwise_xor_u32(&c->e, c->t_u32, lane, b);
                ok = op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, j, max_lane);
            }
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
            /* the lane in the invocation's own 32, on a subgroup of 32 or 64 */
            u32 source =
                spv_emit_bitwise_or_u32(&c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, id, cu(c, ~31u)),
                                        spv_emit_bitwise_and_u32(&c->e, c->t_u32, j, cu(c, 31)));
            u32 value = rd(c, sm70_field(in, 24, 8));
            u32 got = SPV(&c->e, 345 /* OpGroupNonUniformShuffle */, c->t_u32, cu(c, 3), value, source);
            wr(c, in->dst, spv_emit_select(&c->e, c->t_u32, ok, got, value));
            pwr(c, sm70_field(in, 81, 3), ok, 0);
            return;
        }
        case SM70_VOTE:
        case SM70_UVOTE: {
            /* NAK's OpVote: the ballot of the predicate at 87-90 into the destination, and ALL (0), ANY (1) or EQ
               (2, bits 72-73) of it into the predicate at 81-83. The ballot is of the invocation's 32 lanes */
            u32 pred = prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), 0), how = sm70_field(in, 72, 2);
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
            spv_emit_capability(&c->e, 62 /* GroupNonUniformVote */);
            u32 ballot = ballot32(c, pred);
            if (op == SM70_UVOTE)
                uwr(c, sm70_field(in, 16, 6), ballot);
            else
                wr(c, in->dst, ballot);
            if (how > 2) {
                fail(c, "VOTE mode %u", how);
                return;
            }
            u32 vote = SPV(&c->e,
                           how == 0   ? 334
                           : how == 1 ? 335
                                      : 336 /* All, Any, AllEqual */,
                           c->t_bool, cu(c, 3), pred);
            pwr(c, sm70_field(in, 81, 3), vote, op == SM70_UVOTE);
            return;
        }
        case SM70_MATCH: {
            /* NAK's OpMatch: the lanes of the invocation's 32 whose value (a pair with bit 73) is the same. ANY
               (bit 79) gives them; ALL gives all the active lanes when they all are, else 0, and the predicate */
            int any = (int)sm70_field(in, 79, 1), pair = (int)sm70_field(in, 73, 1);
            u32 r = sm70_field(in, 24, 8);
            u32 lo = rd(c, r), hi = pair ? rd(c, r >= SM70_RZ ? r : r + 1) : 0;
            u32 id = system_value(c, 0), base = spv_emit_bitwise_and_u32(&c->e, c->t_u32, id, cu(c, ~31u));
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
            u32 mask = c->zero;
            for (u32 j = 0; j < 32; j++) {
                u32 lane = spv_emit_bitwise_or_u32(&c->e, c->t_u32, base, cu(c, j));
                u32 same = spv_emit_i_equal(&c->e, c->t_bool, SPV(&c->e, 345, c->t_u32, cu(c, 3), lo, lane), lo);
                if (pair)
                    same = op2(c, SPV_OP_LOGICAL_AND, c->t_bool, same,
                               spv_emit_i_equal(&c->e, c->t_bool, SPV(&c->e, 345, c->t_u32, cu(c, 3), hi, lane), hi));
                mask = spv_emit_bitwise_or_u32(&c->e, c->t_u32, mask,
                                               spv_emit_select(&c->e, c->t_u32, same, cu(c, 1u << j), c->zero));
            }
            u32 active = ballot32(c, c->true_v);
            mask = spv_emit_bitwise_and_u32(&c->e, c->t_u32, mask, active);
            if (any)
                wr(c, in->dst, mask);
            else {
                u32 all = spv_emit_i_equal(&c->e, c->t_bool, mask, active);
                wr(c, in->dst, spv_emit_select(&c->e, c->t_u32, all, mask, c->zero));
                pwr(c, sm70_field(in, 81, 3), all, 0);
            }
            return;
        }
        case SM70_ATOMG:
        case SM70_ATOMG_NOU:
        case SM70_RED: {
            /* NAK's OpAtom on global memory: the operation at 87-90 (0 add, 1 min, 2 max, 5 and, 6 or, 7 xor, 8
               exchange), the type at 73-75 (0 u32, 1 s32), the data at 32; RED returns nothing */
            static const u16 ops[2][9] = {
                {SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_U_MIN, SPV_OP_ATOMIC_U_MAX, 0, 0, SPV_OP_ATOMIC_AND,
                 SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR, SPV_OP_ATOMIC_EXCHANGE},
                {SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_S_MIN, SPV_OP_ATOMIC_S_MAX, 0, 0, SPV_OP_ATOMIC_AND,
                 SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR, SPV_OP_ATOMIC_EXCHANGE}};
            u32 how = sm70_field(in, 87, op == SM70_RED ? 3 : 4), type = sm70_field(in, 73, 3);
            if (type > 1 || how > 8 || !ops[type][how]) {
                fail(c, "%s of type %u, operation %u", sm70_ops[op].name, type, how);
                return;
            }
            u32 address, buffer = global_address(c, in, op != SM70_ATOMG_NOU, 1, &address);
            if (!buffer) return;
            u32 old = spv_emit_atomic_u32(&c->e, c->t_u32, ops[type][how], mem_ptr(c, buffer, address, 0),
                                          rd(c, sm70_field(in, 32, 8)));
            if (op != SM70_RED) wr(c, in->dst, old);
            return;
        }
        case SM70_REDUX: {
            /* NAK's OpRedux: of the source over the invocation's 32 lanes, into a uniform register: 78-80 0 and, 1
               or, 2 xor, 3 sum, 4 min, 5 max, bit 73 signed. A clustered reduction of 32 keeps it to those lanes */
            static const u16 ops[6] = {359, 360, 361, 349, 354, 357}; /* BitwiseAnd, Or, Xor, IAdd, UMin, UMax */
            u32 how = sm70_field(in, 78, 3), opcode;
            if (how > 5) {
                fail(c, "REDUX operation %u", how);
                return;
            }
            opcode = ops[how];
            if (sm70_field(in, 73, 1) && how == 4) opcode = 353; /* SMin */
            if (sm70_field(in, 73, 1) && how == 5) opcode = 356; /* SMax */
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC);
            spv_emit_capability(&c->e, 67 /* GroupNonUniformClustered */);
            u32 v = SPV(&c->e, opcode, c->t_u32, cu(c, 3), 3 /* ClusteredReduce */, rd(c, sm70_field(in, 24, 8)),
                        cu(c, 32));
            uwr(c, sm70_field(in, 16, 6), v);
            return;
        }
        case SM70_MEMBAR:
            /* the device scope, for every scope: acquire and release of buffer, workgroup and image memory */
            spv_emit_memory_barrier(&c->e, c->t_u32, 1, 0x8 | 0x40 | 0x100 | 0x800);
            return;
        case SM70_UMOV: uwr(c, in->dst, src_bits(c, &in->b)); return;
        case SM70_ERRBAR: return;
        case SM70_S2UR: uwr(c, sm70_field(in, 16, 6), system_value(c, sm70_field(in, 72, 8))); return;
        case SM70_IMAD_WIDE:
        case SM70_UIMAD_WIDE: {
            /* NAK's OpIMad64: a * b (bit 73 signed) + the pair at c, into the pair at the destination */
            int is_signed = (int)sm70_field(in, 73, 1);
            u32 t64 = u64_type(c);
            u32 a32 = src_int(c, &in->a), b32 = src_int(c, &in->b), a64, b64;
            if (is_signed) {
                u32 i64 = spv_emit_type_int64(&c->e, 1);
                a64 = spv_emit_bitcast(&c->e, t64,
                                       SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_bitcast(&c->e, c->t_i32, a32)));
                b64 = spv_emit_bitcast(&c->e, t64,
                                       SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_bitcast(&c->e, c->t_i32, b32)));
            } else
                a64 = to64(c, a32), b64 = to64(c, b32);
            u32 creg = in->c.reg, chi;
            if (in->c.kind == SM70_SRC_IMM || in->c.kind == SM70_SRC_CBUF) {
                fail(c, "%s with c an immediate or a constant", sm70_ops[op].name);
                return;
            }
            chi = in->c.kind == SM70_SRC_UREG ? urd(c, creg >= SM70_URZ ? creg : creg + 1)
                                              : rd(c, creg >= SM70_RZ ? creg : creg + 1);
            u32 sum =
                op2(c, SPV_OP_I_ADD, t64, op2(c, SPV_OP_I_MUL, t64, a64, b64), pair64(c, src_bits(c, &in->c), chi));
            u32 d = in->dst;
            dwr(c, in, lo32(c, sum));
            if (uniform && d < SM70_URZ)
                uwr(c, d + 1, hi32(c, sum));
            else if (!uniform && d < SM70_RZ)
                wr(c, d + 1, hi32(c, sum));
            return;
        }
        case SM70_IMAD_HI: {
            /* the high word of a * b (bit 73 signed), plus c */
            int is_signed = (int)sm70_field(in, 73, 1);
            u32 t64 = u64_type(c), a32 = src_int(c, &in->a), b32 = src_int(c, &in->b), product;
            if (is_signed) {
                u32 i64 = spv_emit_type_int64(&c->e, 1);
                product =
                    op2(c, SPV_OP_I_MUL, t64,
                        spv_emit_bitcast(&c->e, t64,
                                         SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_bitcast(&c->e, c->t_i32, a32))),
                        spv_emit_bitcast(&c->e, t64,
                                         SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_bitcast(&c->e, c->t_i32, b32))));
            } else
                product = op2(c, SPV_OP_I_MUL, t64, to64(c, a32), to64(c, b32));
            u32 v = spv_emit_iadd_u32(&c->e, c->t_u32, hi32(c, product), src_int(c, &in->c));
            if (sm70_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_select(&c->e, c->t_u32,
                                                      prd(c, sm70_field(in, 87, 3), sm70_field(in, 90, 1), 0), cu(c, 1),
                                                      c->zero));
            dwr(c, in, v);
            return;
        }
        case SM70_SGXT:
        case SM70_USGXT: {
            /* the low b bits (at most 32) of a, sign (bit 73) or zero extended; 0 bits give 0 */
            u32 bits = spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, src_bits(c, &in->b), cu(c, 32));
            u32 a = src_bits(c, &in->a);
            u32 v = sm70_field(in, 73, 1)
                        ? to_u(c, spv_emit_bitfield_sextract(&c->e, c->t_i32, spv_emit_bitcast(&c->e, c->t_i32, a),
                                                             c->zero, bits))
                        : spv_emit_bitfield_uextract(&c->e, c->t_u32, a, c->zero, bits);
            dwr(c, in, spv_emit_select(&c->e, c->t_u32, spv_emit_i_equal(&c->e, c->t_bool, bits, c->zero), c->zero, v));
            return;
        }
        case SM70_PRMT:
        case SM70_UPRMT: {
            /* the index mode (72-74 = 0): output byte k is byte (sel >> 4k) & 7 of c:a, its sign spread with bit 3 */
            if (sm70_field(in, 72, 3)) {
                fail(c, "PRMT mode %u", sm70_field(in, 72, 3));
                return;
            }
            u32 a = src_bits(c, &in->a), sel = src_bits(c, &in->b), hi = src_bits(c, &in->c), v = c->zero;
            for (u32 k = 0; k < 4; k++) {
                u32 nib = spv_emit_bitfield_uextract(&c->e, c->t_u32, sel, cu(c, 4 * k), cu(c, 4));
                u32 from_c = spv_emit_i_not_equal(&c->e, c->t_bool,
                                                  spv_emit_bitwise_and_u32(&c->e, c->t_u32, nib, cu(c, 4)), c->zero);
                u32 word = spv_emit_select(&c->e, c->t_u32, from_c, hi, a);
                u32 shift = spv_emit_shift_left_u32(&c->e, c->t_u32,
                                                    spv_emit_bitwise_and_u32(&c->e, c->t_u32, nib, cu(c, 3)), cu(c, 3));
                u32 byte = spv_emit_bitfield_uextract(&c->e, c->t_u32, word, shift, cu(c, 8));
                u32 spread = spv_emit_i_not_equal(&c->e, c->t_bool,
                                                  spv_emit_bitwise_and_u32(&c->e, c->t_u32, nib, cu(c, 8)), c->zero);
                u32 sign = spv_emit_select(
                    &c->e, c->t_u32,
                    spv_emit_i_not_equal(&c->e, c->t_bool, spv_emit_bitwise_and_u32(&c->e, c->t_u32, byte, cu(c, 0x80)),
                                         c->zero),
                    cu(c, 0xff), c->zero);
                byte = spv_emit_select(&c->e, c->t_u32, spread, sign, byte);
                v = spv_emit_bitwise_or_u32(&c->e, c->t_u32, v,
                                            spv_emit_shift_left_u32(&c->e, c->t_u32, byte, cu(c, 8 * k)));
            }
            dwr(c, in, v);
            return;
        }
        case SM70_HADD2: {
            /* two halves added: bit 77 saturate, 78 a float result, 80 FTZ. The swizzles: a's at 74-75, b's at 60-61
               (NAK: 0 none, 2 the low half twice, 3 the high one twice) */
            /* .F32 (bit 78): the low half's sum as a float, as ptxas's HADD2.F32 R, -RZ, R.H0_H0 widens a half */
            int as_f32 = (int)sm70_field(in, 78, 1);
            const Sm70Src* b = second(in);
            u32 sw[2] = {sm70_field(in, 74, 2), b->kind == SM70_SRC_IMM ? 0 : sm70_field(in, 60, 2)};
            const Sm70Src* src[2] = {&in->a, b};
            u32 v[2];
            for (u32 k = 0; k < 2; k++) {
                if (sw[k] == 1) {
                    fail(c, "HADD2 with a float operand");
                    return;
                }
                u32 pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, src[k]));
                if (sw[k] >= 2) {
                    u32 h = spv_emit_vec4_extract(&c->e, c->t_f32, pair, sw[k] - 2);
                    pair = spv_emit_vec2_construct(&c->e, c->t_v2f, h, h);
                }
                if (src[k]->kind != SM70_SRC_IMM && src[k]->abs) pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, pair);
                if (src[k]->kind != SM70_SRC_IMM && src[k]->neg) pair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, pair);
                v[k] = pair;
            }
            u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_v2f, v[0], v[1]));
            if (sm70_field(in, 77, 1))
                sum = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_CLAMP, sum,
                              spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 0.0f), cf(c, 0.0f)),
                              spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 1.0f), cf(c, 1.0f)));
            dwr(c, in,
                as_f32 ? to_u(c, spv_emit_vec4_extract(&c->e, c->t_f32, sum, 0))
                       : SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, sum));
            return;
        }
        case SM70_ULDC_IDX: {
            /* ULDC UR, c[bank][UR + offset]: the uniform register at 24, the offset 38-53 signed */
            u32 type = sm70_field(in, 73, 3), bank = sm70_field(in, 54, 5), off = sm70_field(in, 38, 16);
            if (type != 4 && type != 5) {
                fail(c, "ULDC (indexed) of type %u", type);
                return;
            }
            u32 bias = off & 0x8000u ? off | 0xffff0000u : off, d = sm70_field(in, 16, 6);
            u32 index = urd(c, sm70_field(in, 24, 6));
            uwr(c, d, cbuf_load(c, bank, bias, index));
            if (type == 5 && d < SM70_URZ) uwr(c, d + 1, cbuf_load(c, bank, bias + 4, index));
            return;
        }
        case SM70_ULDC: {
            u32 type = sm70_field(in, 73, 3), bank = sm70_field(in, 54, 5), off = sm70_field(in, 38, 16);
            if (sm70_field(in, 91, 1) || (type != 4 && type != 5)) {
                fail(c, "ULDC of type %u", type);
                return;
            }
            u32 d = sm70_field(in, 16, 6);
            uwr(c, d, cbuf_load(c, bank, off, 0));
            if (type == 5 && d < SM70_URZ) uwr(c, d + 1, cbuf_load(c, bank, off + 4, 0));
            return;
        }
        case SM70_LDC: {
            u32 type = sm70_field(in, 73, 3), bank = sm70_field(in, 54, 5), off = sm70_field(in, 38, 16);
            u32 r = sm70_field(in, 24, 8);
            if (sm70_field(in, 78, 2) || (type != 4 && type != 5)) {
                fail(c, "LDC of type %u, mode %u", type, sm70_field(in, 78, 2));
                return;
            }
            u32 index = r < SM70_RZ ? rd(c, r) : 0;
            wr(c, in->dst, cbuf_load(c, bank, off, index));
            if (type == 5 && in->dst < SM70_RZ) wr(c, in->dst + 1, cbuf_load(c, bank, off + 4, index));
            return;
        }
        case SM70_LDG:
        case SM70_STG:
        case SM70_LDG_NOU:
        case SM70_STG_NOU: {
            int load = op == SM70_LDG || op == SM70_LDG_NOU;
            u32 address, buffer = global_address(c, in, op == SM70_LDG || op == SM70_STG, !load, &address);
            if (!buffer) return;
            if (load)
                mem_load(c, buffer, address, sm70_field(in, 73, 3), in->dst, 0);
            else
                mem_store(c, buffer, address, sm70_field(in, 73, 3), sm70_field(in, 32, 8));
            return;
        }
        case SM70_LDS:
        case SM70_STS:
        case SM70_STS_NOU: {
            u32 ur = op == SM70_STS_NOU ? SM70_URZ : sm70_field(in, op == SM70_LDS ? 32 : 64, 6);
            /* the register times 1, 4, 8 or 16 (bits 78-79, NAK's OffsetStride), the uniform register and the
               signed 24-bit offset */
            static const u8 shifts[4] = {0, 2, 3, 4};
            u32 imm = sm70_field(in, 40, 24);
            u32 bias = imm & 0x800000u ? imm | 0xff000000u : imm;
            u32 base = rd(c, sm70_field(in, 24, 8)), shift = shifts[sm70_field(in, 78, 2)];
            if (shift) base = spv_emit_shift_left_u32(&c->e, c->t_u32, base, cu(c, shift));
            u32 address = spv_emit_iadd_u32(&c->e, c->t_u32, base, cu(c, bias));
            if (ur < SM70_URZ) address = spv_emit_iadd_u32(&c->e, c->t_u32, address, urd(c, ur));
            shared_var(c);
            if (op == SM70_LDS)
                mem_load(c, 0, address, sm70_field(in, 73, 3), in->dst, 0);
            else
                mem_store(c, 0, address, sm70_field(in, 73, 3), sm70_field(in, 32, 8));
            return;
        }
        case SM70_ALD:
        case SM70_AST: {
            u32 addr = sm70_field(in, 40, 10), comps = sm70_field(in, 74, 2) + 1;
            u32 vtx = sm70_field(in, op == SM70_ALD ? 32 : 64, 8);
            if (sm70_field(in, 24, 8) != SM70_RZ || vtx != SM70_RZ || sm70_field(in, 76, 2) ||
                (op == SM70_ALD && sm70_field(in, 79, 1))) {
                fail(c, "%s indexed, of a patch or of an output", sm70_ops[op].name);
                return;
            }
            for (u32 k = 0; k < comps; k++) {
                if (op == SM70_ALD) {
                    if (in->dst < SM70_RZ) wr(c, in->dst + k, attr_load(c, addr + 4 * k));
                } else {
                    u32 data = sm70_field(in, 32, 8);
                    attr_store(c, addr + 4 * k, rd(c, data >= SM70_RZ ? data : data + k));
                }
            }
            return;
        }
        case SM70_IPA: {
            u32 addr = sm70_field(in, 64, 8) * 4;
            /* bits 76-77, NAK's InterpLoc: 0 the default, 1 the centroid, 2 at an offset */
            u32 loc = sm70_field(in, 76, 2);
            if (sm70_field(in, 32, 8) != SM70_RZ || sm70_field(in, 24, 8) != 0 || sm70_field(in, 78, 2) > 1 ||
                loc > 1) {
                fail(c, "IPA indexed, at an offset or of frequency %u", sm70_field(in, 78, 2));
                return;
            }
            u32 v;
            if (addr >= 0x70 && addr < 0x80)
                v = frag_coord(c, (addr >> 2) & 3);
            else if (addr >= 0x80 && addr < 0x280) {
                u32 i = (addr - 0x80) >> 4;
                v = to_f(c, input_load(c, i, (addr >> 2) & 3));
                if (loc == 1 && !(c->centroid_mask & (1u << i))) {
                    /* a variable has one location: centroid when any IPA of the input asks for it */
                    spv_emit_decorate(&c->e, c->in_attr[i], 16 /* Centroid */, NULL, 0);
                    c->centroid_mask |= 1u << i;
                }
                /* The hardware interpolates attr/w for a perspective input, and the program multiplies by w (the
                   reciprocal of a[0x7c]). Vulkan's input is attr itself: times the interpolated 1/w, FragCoord.w */
                if (interp_mode(c, i) == 2 && sm70_field(in, 78, 2) == 0) {
                    v = spv_emit_fmul(&c->e, c->t_f32, v, frag_coord(c, 3));
                    spv_emit_no_contraction(&c->e, v);
                }
            } else if (addr == 0x3fc) {
                /* NAK's NAK_ATTR_FRONT_FACE: all ones for a front face, as spirv_translate.c reads Maxwell's */
                if (!c->front_facing) {
                    c->front_facing = spv_emit_builtin_input_u32(&c->e, c->t_bool, 17);
                    add_interface(c, c->front_facing);
                }
                v = to_f(c, spv_emit_select(&c->e, c->t_u32, spv_emit_load(&c->e, c->t_bool, c->front_facing),
                                            cu(c, 0xffffffffu), c->zero));
            } else if (addr == 0x2e0 || addr == 0x2e4) {
                /* NAK's NAK_ATTR_POINT_SPRITE_S and _T: PointCoord. The Maxwell path flips T with the sprite's
                   origin, a push constant of the fragment stage that this translation does not have */
                if (!c->point_coord) {
                    c->point_coord = spv_emit_interface_variable(&c->e, 1, c->t_v2f, c->t_u32, 0, 16, 0, 0);
                    add_interface(c, c->point_coord);
                }
                v = spv_emit_vec4_extract(&c->e, c->t_f32, spv_emit_load(&c->e, c->t_v2f, c->point_coord),
                                          (addr >> 2) & 1);
            } else {
                fail(c, "IPA of attribute 0x%x", addr);
                return;
            }
            wr(c, in->dst, to_u(c, v));
            return;
        }
        case SM70_TEX:
        case SM70_TLD4:
        case SM70_TMML: texture(c, in); return;
        case SM70_TLD: texel_fetch(c, in); return;
        case SM70_TXQ: texture_query(c, in); return;
        case SM70_SULD_P:
        case SM70_SUST_P:
        case SM70_SURED: surface(c, in); return;
        case SM70_KILL:
            spv_emit_capability(&c->e, SPV_CAPABILITY_DEMOTE_TO_HELPER_INVOCATION);
            spv_emit_demote(&c->e);
            return;
        case SM70_BAR:
            if (sm70_field(in, 77, 2) || in->hi & 0xfffffffull) {
                fail(c, "BAR other than BAR.SYNC 0");
                return;
            }
            spv_emit_control_barrier(&c->e, c->t_u32);
            return;
        default: fail(c, "%s", sm70_ops[op].name); return;
    }
}

/* ---- structured control flow ---- */

/* Writing the structure (shader_structure_graph): each scope's header, body, continue target and merge labels, made
   when it opens, then the open selections' merge and else labels */
typedef struct {
    const ShStructure* st;
    u32 next, *label, selections, flag, body_start, body_end;
    u8* used;
    int open;
} StructEmit;
enum { SE_DONE, SE_BODY, SE_END };

static void se_branch(Ctx* c, StructEmit* se, u32 label) {
    spv_emit_branch(&c->e, label);
    se->open = 0;
}
static void se_break(Ctx* c, StructEmit* se, u32 scope) {
    se->used[scope] = 1;
    se_branch(c, se, se->label[4 * scope + 3]);
}
static void se_ensure_open(Ctx* c, StructEmit* se) {
    if (se->open) return;
    spv_emit_label(&c->e, spv_emit_id(&c->e));
    se->open = 1;
}
static void se_pending(Ctx* c, StructEmit* se, u32 scope) {
    const ShsScope* s = &se->st->scope[scope];
    u32 parent = s->parent - 1;
    u32 t = spv_emit_load(&c->e, c->t_u32, se->flag);
    u32 go = spv_emit_id(&c->e), pending = spv_emit_id(&c->e);
    u32 here = cu(c, parent + 1);
    u32 set = spv_emit_i_not_equal(&c->e, c->t_bool, t, c->zero); /* before the merge, which precedes the branch */
    spv_emit_selection_merge(&c->e, go);
    spv_emit_branch_conditional(&c->e, set, pending, go);
    spv_emit_label(&c->e, pending);
    if (!se->st->scope[parent].loop) {
        if (s->arrive && s->beyond)
            spv_emit_store(&c->e, se->flag,
                           spv_emit_select(&c->e, c->t_u32, spv_emit_i_equal(&c->e, c->t_bool, t, here), c->zero, t));
        else if (s->arrive)
            spv_emit_store(&c->e, se->flag, c->zero);
        se_break(c, se, parent);
    } else if (s->arrive && s->beyond) {
        u32 again = spv_emit_id(&c->e), out = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
        u32 arrived = spv_emit_i_equal(&c->e, c->t_bool, t, here);
        spv_emit_selection_merge(&c->e, merge);
        spv_emit_branch_conditional(&c->e, arrived, again, out);
        spv_emit_label(&c->e, again);
        spv_emit_store(&c->e, se->flag, c->zero);
        spv_emit_branch(&c->e, se->label[4 * parent + 2]);
        spv_emit_label(&c->e, out);
        se_break(c, se, parent);
        spv_emit_label(&c->e, merge);
        spv_emit_void_op(&c->e, SPV_OP_UNREACHABLE, NULL, 0);
    } else if (s->arrive) {
        spv_emit_store(&c->e, se->flag, c->zero);
        spv_emit_branch(&c->e, se->label[4 * parent + 2]);
    } else
        se_break(c, se, parent);
    spv_emit_label(&c->e, go);
    se->open = 1;
}
static int structure_step(Ctx* c, StructEmit* se) {
    const ShStructure* st = se->st;
    while (se->next < st->ntokens) {
        const ShsToken* t = &st->token[se->next++];
        u32* sel = se->label + 4 * st->nscopes + 2 * (se->selections ? se->selections - 1 : 0);
        u8* sel_used = se->used + st->nscopes + (se->selections ? se->selections - 1 : 0);
        u32* l = se->label + 4 * t->a;
        switch (t->kind) {
            case SHS_BODY:
                se_ensure_open(c, se);
                se->body_start = t->a;
                se->body_end = t->b;
                return SE_BODY;
            case SHS_END: se_ensure_open(c, se); return SE_END;
            case SHS_RETURNED: se->open = 0; break;
            case SHS_OPEN_BLOCK:
            case SHS_OPEN_LOOP:
                for (u32 k = 0; k < 4; k++) l[k] = spv_emit_id(&c->e);
                if (se->open) spv_emit_branch(&c->e, l[0]);
                spv_emit_label(&c->e, l[0]);
                spv_emit_loop_merge(&c->e, l[3], l[2]);
                spv_emit_branch(&c->e, l[1]);
                spv_emit_label(&c->e, l[1]);
                se->open = 1;
                break;
            case SHS_CLOSE:
                if (se->open) se_break(c, se, t->a);
                spv_emit_label(&c->e, l[2]);
                spv_emit_branch(&c->e, l[0]);
                spv_emit_label(&c->e, l[3]);
                if (!se->used[t->a]) {
                    spv_emit_void_op(&c->e, SPV_OP_UNREACHABLE, NULL, 0);
                    se->open = 0;
                } else {
                    se->open = 1;
                    if (st->scope[t->a].check) se_pending(c, se, t->a);
                }
                break;
            case SHS_IF: {
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->e);
                sel[1] = spv_emit_id(&c->e);
                *sel_used = 0;
                u32 then = spv_emit_id(&c->e);
                spv_emit_selection_merge(&c->e, sel[0]);
                spv_emit_branch_conditional(&c->e, c->branch_cond ? c->branch_cond : c->true_v, then, sel[1]);
                spv_emit_label(&c->e, then);
                se->open = 1;
                break;
            }
            case SHS_ELSE:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->e, sel[1]);
                se->open = 1;
                break;
            case SHS_ENDIF:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->e, sel[0]);
                se->open = *sel_used;
                if (!se->open) spv_emit_void_op(&c->e, SPV_OP_UNREACHABLE, NULL, 0);
                se->selections--;
                break;
            case SHS_BREAK:
                se_ensure_open(c, se);
                if (t->b) spv_emit_store(&c->e, se->flag, cu(c, t->b));
                se_break(c, se, t->a);
                break;
            case SHS_CONTINUE:
                se_ensure_open(c, se);
                se_branch(c, se, l[2]);
                break;
            case SHS_SWITCH: {
                /* the cases' instructions, from their tokens up to the switch's end */
                u32 literals[64], labels[64], count = 0, depth = 0;
                for (u32 k = se->next; k < st->ntokens && count < 64; k++) {
                    u32 kind = st->token[k].kind;
                    if (kind == SHS_SWITCH) depth++;
                    if (kind == SHS_ENDSWITCH && !depth--) break;
                    if (kind == SHS_CASE && !depth) literals[count++] = st->token[k].b;
                }
                se_ensure_open(c, se);
                sel += se->selections ? 2 : 0;
                sel_used += se->selections ? 1 : 0;
                se->selections++;
                sel[0] = spv_emit_id(&c->e);
                sel[1] = spv_emit_id(&c->e); /* the default's label, the cases' from the next id on */
                for (u32 k = 0; k < count; k++) labels[k] = spv_emit_id(&c->e);
                *sel_used = 0;
                spv_emit_selection_merge(&c->e, sel[0]);
                spv_emit_switch_u32(&c->e, c->branch_target, sel[1], literals, labels, count);
                se->open = 0;
                break;
            }
            case SHS_CASE:
            case SHS_DEFAULT:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->e, t->kind == SHS_DEFAULT ? sel[1] : sel[1] + 1 + t->a);
                se->open = 1;
                break;
            case SHS_ENDSWITCH:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                spv_emit_label(&c->e, sel[0]);
                se->open = *sel_used;
                if (!se->open) spv_emit_void_op(&c->e, SPV_OP_UNREACHABLE, NULL, 0);
                se->selections--;
                break;
            default: break;
        }
    }
    return SE_DONE;
}

/* BRX R, offset (CuAssembler sm_86): to the byte R + offset + the next instruction's, where the offset is in 4-byte
   units from bit 33 to 81, signed. ptxas writes minus the next instruction's address, so that R is the target's own.
   The constant added to R, as a u32 */
static u32 brx_bias(const Sm70Insn* in, u32 i) {
    u64 v = (in->lo >> 33) | (in->hi & 0x3ffffull) << 31; /* 49 bits */
    if (v >> 48) v |= ~0ull << 49;
    return (u32)(v * 4 + ((u64)i + 1) * 16);
}
/* The targets of the BRX at i: the jump table of the LDC before it that writes its register, read through
   g_shader_cb_read as spirv_translate.c reads Maxwell's, up to the first word that is no target. The count, or 0 */
static u32 brx_targets(Ctx* c, u32 i, u32* targets, u32 max) {
    const Sm70Insn* in = &c->in[i];
    u32 reg = sm70_field(in, 24, 8), bias = brx_bias(in, i);
    if (!(in->pred == SM70_PT && !in->pred_not)) {
        fail(c, "BRX on a predicate");
        return 0;
    }
    if (!g_shader_cb_read) {
        fail(c, "BRX without a reader of constant buffers");
        return 0;
    }
    for (u32 k = i; k-- > 0 && k + 32 > i;) {
        const Sm70Insn* w = &c->in[k];
        if (w->op == SM70_BRA || w->op == SM70_EXIT || w->op == SM70_BRX) break;
        if (w->dst != reg || sm70_ops[w->op].kind == SM70_K_UALU) continue;
        if (w->op != SM70_LDC || sm70_field(w, 73, 3) != 4 || sm70_field(w, 78, 2)) break;
        u32 bank = sm70_field(w, 54, 5), off = sm70_field(w, 38, 16), count = 0;
        for (; count < max; count++) {
            u32 entry;
            if (!g_shader_cb_read(bank, off + 4 * count, &entry)) break;
            u32 at = entry + bias;
            if ((at & 15) || at / 16 >= c->n) break;
            targets[count] = at / 16;
        }
        if (!count) {
            u32 first = 0;
            int read = g_shader_cb_read(bank, off, &first);
            fail(c, "BRX with no jump table in c[%u][0x%x] (%s 0x%x)", bank, off, read ? "word" : "no word, unread",
                 first);
        }
        return count;
    }
    fail(c, "BRX without the LDC of its jump table");
    return 0;
}

/* the blocks that BRA, BRX and EXIT end, and their structure */
static ShStructure* build_structure(Ctx* c) {
    u32 n = c->n, ncases = 0;
    u8* leader = calloc(n + 1, 1);
    u32* cases = calloc(64 * 64, sizeof *cases);   /* the jump tables' targets, 64 BRX of 64 */
    u32* case_of = calloc(n + 1, sizeof *case_of); /* a BRX's first target + 1, by instruction */
    u32* case_count = calloc(n + 1, sizeof *case_count);
    if (!leader || !cases || !case_of || !case_count) {
        free(leader), free(cases), free(case_of), free(case_count);
        return NULL;
    }
    leader[0] = 1;
    for (u32 i = 0; i < n && !c->failed; i++) {
        const Sm70Insn* in = &c->in[i];
        int never = in->pred == SM70_PT && in->pred_not;
        if (in->op == SM70_BRA && !never) {
            u32 t = sm70_branch_target(in, i);
            if (t >= n) {
                fail(c, "branch out of the program");
                break;
            }
            leader[t] = 1;
            leader[i + 1] = 1;
        } else if (in->op == SM70_BRX && !never) {
            if (ncases + 64 > 64 * 64) {
                fail(c, "more than 64 BRX");
                break;
            }
            u32 count = brx_targets(c, i, cases + ncases, 64);
            case_of[i] = ncases + 1;
            case_count[i] = count;
            for (u32 k = 0; k < count; k++) leader[cases[ncases + k]] = 1;
            ncases += count;
            leader[i + 1] = 1;
        } else if ((in->op == SM70_EXIT && in->pred == SM70_PT && !in->pred_not) || in->op == SM70_RET)
            leader[i + 1] = 1;
    }
    u32 nb = 0;
    for (u32 i = 0; i < n; i++) nb += leader[i];
    /* the blocks: their starts, successors (two words each), the switches' first case (nb + 1 words) and
       terminators */
    u32* start = calloc((size_t)nb * 4 + 2, sizeof *start);
    u8* term = calloc(nb + 1, 1);
    u32* block_of = calloc(n + 1, sizeof *block_of);
    u32* sw_targets = calloc((size_t)ncases + 1, sizeof *sw_targets);
    ShStructure* st = NULL;
    if (!c->failed && start && term && block_of && sw_targets) {
        u32 *succ = start + nb, *sw_first = succ + 2 * nb, nsw = 0;
        for (u32 i = 0, b = 0; i < n; i++) {
            if (leader[i]) start[b++] = i;
            block_of[i] = b - 1;
        }
        for (u32 b = 0; b < nb; b++) {
            u32 end = b + 1 < nb ? start[b + 1] : n, last = end - 1;
            const Sm70Insn* in = &c->in[last];
            int always = in->pred == SM70_PT && !in->pred_not, never = in->pred == SM70_PT && in->pred_not;
            sw_first[b] = nsw;
            if (in->op == SM70_BRA && !never) {
                succ[2 * b] = block_of[sm70_branch_target(in, last)];
                if (always || succ[2 * b] == b + 1) /* as terminators() takes a branch to the next block */
                    term[b] = SHT_GOTO;
                else if (b + 1 < nb) {
                    term[b] = SHT_COND;
                    succ[2 * b + 1] = b + 1;
                } else
                    term[b] = SHT_GOTO; /* a conditional branch off the end: the program can't run off it */
            } else if (in->op == SM70_BRX && case_of[last]) {
                term[b] = SHT_SWITCH;
                for (u32 k = 0; k < case_count[last]; k++) sw_targets[nsw++] = block_of[cases[case_of[last] - 1 + k]];
            } else if ((in->op == SM70_EXIT && always) || in->op == SM70_RET)
                term[b] = SHT_EXIT; /* a RET that the inlining left: only in a function that nothing reaches */
            else if (b + 1 < nb) {
                term[b] = SHT_FALL;
                succ[2 * b] = b + 1;
            } else
                term[b] = SHT_END;
        }
        sw_first[nb] = nsw;
        st = shader_structure_graph(n, nb, start, term, succ, sw_first, sw_targets);
        if (!st) fail(c, "control flow the structure can't take");
    }
    free(sw_targets);
    free(block_of);
    free(term);
    free(start);
    free(case_count);
    free(case_of);
    free(cases);
    free(leader);
    return st;
}

/* a BRA from instruction ip to target, on the guard of in (sm70_set_rel_offset's field, bits 34-81) */
static Sm70Insn make_bra(const Sm70Insn* in, u32 ip, u32 target) {
    u64 field = (((u64)target - (u64)ip - 1) * 4) & ((1ull << 48) - 1);
    u64 lo = 0x947u | (u64)in->pred << 12 | (u64)in->pred_not << 15 | field << 34;
    u64 hi = field >> 30 | 7ull << 23 | 7ull << 46 | 7ull << 49; /* condition PT, no barriers */
    Sm70Insn out;
    sm70_decode(lo, hi, &out);
    return out;
}
/* CALL.REL and RET.REL inlined: each CALL becomes a BRA to a copy of its function (from its target up to the BRA
   to itself that ends it), whose RETs become BRAs back to the instruction after the CALL. ptxas's RET returns
   there, through the address that the caller writes into the RET's register. The copies go after the program, and
   the function's own code stays where nothing reaches it. Returns the new instructions (malloc'd) and their count,
   or NULL */
static Sm70Insn* inline_calls(Ctx* c, Sm70Insn* insn, u32* count) {
    u32 n = *count, cap = n * 2 + 64;
    Sm70Insn* out = malloc((size_t)cap * sizeof *out);
    if (!out) return NULL;
    memcpy(out, insn, (size_t)n * sizeof *out);
    for (u32 i = 0, copies = 0; i < n; i++) {
        if (out[i].op != SM70_CALL) continue;
        u32 entry = sm70_branch_target(&out[i], i), end = entry;
        while (end < n && !(out[end].op == SM70_BRA && out[end].pred == SM70_PT && !out[end].pred_not &&
                            sm70_branch_target(&out[end], end) == end))
            end++;
        if (entry >= *count || end >= *count || ++copies > 64) {
            fail(c, "CALL to 0x%x that the inlining can't take", entry * 16);
            free(out);
            return NULL;
        }
        /* ptxas also leaves a loop with @P CALL.REL.NOINC to the code after it, which no RET ends: a branch */
        int returns = 0;
        for (u32 k = entry; k <= end; k++) returns |= insn[k].op == SM70_RET;
        if (!returns) {
            out[i] = make_bra(&out[i], i, entry);
            continue;
        }
        u32 len = end - entry + 1;
        if ((size_t)n + len > 65536) {
            fail(c, "CALLs inlined past 65536 instructions");
            free(out);
            return NULL;
        }
        if (n + len > cap) {
            cap = (n + len) * 2;
            Sm70Insn* grown = realloc(out, (size_t)cap * sizeof *out);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        /* the copy keeps its branches' relative offsets, which stay inside it; its own CALLs come later in this
           loop, as it is after i */
        memcpy(out + n, insn + entry, (size_t)len * sizeof *out);
        for (u32 k = 0; k < len; k++) {
            const Sm70Insn* in = &out[n + k];
            if (in->op == SM70_CALL) {
                /* the target of a CALL in the copy, relative, now points into the original: keep it so */
                u32 t = sm70_branch_target(in, entry + k);
                Sm70Insn call = make_bra(in, n + k, t);
                call.op = SM70_CALL;
                out[n + k] = call;
            } else if (in->op == SM70_RET)
                out[n + k] = make_bra(in, n + k, i + 1);
            else if (in->op == SM70_BRA || in->op == SM70_BSSY) {
                /* a branch out of the copied function keeps its target in the original */
                u32 t = sm70_branch_target(in, entry + k);
                if (t < entry || t > end) {
                    Sm70Insn far = make_bra(in, n + k, t);
                    if (in->op == SM70_BSSY) far = *in; /* BSSY's target only names a convergence point */
                    out[n + k] = far;
                }
            }
        }
        out[i] = make_bra(&out[i], i, n);
        n += len;
    }
    *count = n;
    return out;
}

/* the highest register an instruction's fields can name, of the 8-bit register fields */
static u32 max_register(const Sm70Insn* in) {
    static const u8 fields[] = {16, 24, 32, 64};
    u32 top = 0;
    for (u32 k = 0; k < sizeof fields; k++) {
        u32 r = sm70_field(in, fields[k], 8);
        if (r < SM70_RZ && r > top) top = r;
    }
    return top;
}

u32* sm70_to_spirv(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count) {
    if (!code || !si || !word_count || stage < ST_VS || stage > ST_CS || bytes < 16) return NULL;
    if (stage != ST_CS && !sph) return NULL;
    if (stage != ST_VS && stage != ST_FS && stage != ST_CS) {
        si->unsupported++;
        snprintf(si->first_unsupported, sizeof si->first_unsupported, "stage %d", stage);
        return NULL;
    }
    u32 n = bytes / 16;
    Ctx* c = calloc(1, sizeof *c);
    u64* words = malloc((size_t)n * 16);
    Sm70Insn* insn = malloc((size_t)n * sizeof *insn);
    u32 zero_sph[32] = {0};
    if (!c || !words || !insn) {
        free(c);
        free(words);
        free(insn);
        return NULL;
    }
    c->si = si;
    c->stage = stage;
    c->sph = sph ? sph : zero_sph;
    c->in = insn;
    c->n = n;
    si->stage = stage;
    if (sph)
        for (u32 i = 0; i < 20; i++) si->sph[i] = sph[i];
    sm70_load(code, n, words);
    u32 top = 0;
    for (u32 i = 0; i < n; i++) {
        if (!sm70_decode(words[2 * i], words[2 * i + 1], &insn[i]))
            fail(c, "unknown opcode 0x%03x", (u32)(words[2 * i] & 0xfff));
        else if (!stage_ok(c, &insn[i]))
            fail(c, "%s in stage %d", sm70_ops[insn[i].op].name, stage);
        u32 r = max_register(&insn[i]);
        if (r > top) top = r;
    }
    /* the CALLs inlined: the program as the translation sees it */
    for (u32 i = 0; i < n && !c->failed; i++)
        if (insn[i].op == SM70_CALL) {
            Sm70Insn* flat = inline_calls(c, insn, &n);
            if (flat) {
                free(insn);
                insn = flat;
                c->in = insn;
                c->n = n;
            }
            break;
        }
    if (!c->failed && !track_globals(c)) fail(c, "out of memory");
    ShStructure* st = c->failed ? NULL : build_structure(c);
    u32* out = NULL;
    if (!c->failed && st) {
        SpvEmitter* e = &c->e;
        spv_emit_init(e, (size_t)n * 40 + 512);
        spv_emit_shader_capability(e);
        c->glsl = spv_emit_glsl450_import(e);
        c->t_void = spv_emit_type_void(e);
        c->t_bool = spv_emit_type_bool(e);
        c->t_u32 = spv_emit_type_u32(e);
        c->t_i32 = spv_emit_type_i32(e);
        c->t_f32 = spv_emit_type_f32(e);
        c->t_v2f = spv_emit_type_vec2(e, c->t_f32);
        c->t_v3f = spv_emit_type_vec3(e, c->t_f32);
        c->t_v4f = spv_emit_type_vec4(e, c->t_f32);
        c->t_v4u = spv_emit_type_vec4(e, c->t_u32);
        c->t_v3u = spv_emit_type_vec3(e, c->t_u32);
        c->t_v2u = spv_emit_type_vec2(e, c->t_u32);
        c->zero = cu(c, 0);
        c->true_v = spv_emit_constant_bool(e, c->t_bool, 1);
        c->false_v = spv_emit_constant_bool(e, c->t_bool, 0);
        u32 fn = spv_emit_id(e);
        spv_emit_function_void(e, c->t_void, fn, spv_emit_type_function_void(e, c->t_void));
        spv_emit_label(e, spv_emit_id(e));
        u32 reg_ptr = spv_emit_type_function_pointer(e, c->t_u32),
            pred_ptr = spv_emit_type_function_pointer(e, c->t_bool);
        /* the registers up to the highest named and the next three (vector operands), and the predicates */
        for (u32 r = 0; r <= top + 3 && r < SM70_RZ; r++)
            c->reg[r] = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        for (u32 r = 0; r < SM70_URZ; r++) c->ureg[r] = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        for (u32 p = 0; p < SM70_PT; p++) {
            c->pred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
            c->upred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
        }
        /* a vertex program's position, made before its code, so that every exit's depth fix sees it */
        for (u32 i = 0; i < n && stage == ST_VS; i++)
            if (insn[i].op == SM70_AST && sm70_field(&insn[i], 40, 10) < 0x80 &&
                sm70_field(&insn[i], 40, 10) + 4 * (sm70_field(&insn[i], 74, 2) + 1) > 0x70)
                position_var(c);
        StructEmit se = {0};
        se.st = st;
        se.open = 1;
        se.label = calloc(4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1, sizeof *se.label + 1);
        if (!se.label) abort();
        se.used = (u8*)(se.label + 4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1);
        if (st->uses_flag) se.flag = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        while (!c->failed) {
            int step = structure_step(c, &se);
            if (step == SE_DONE) break;
            if (step == SE_END) {
                epilogue(c);
                spv_emit_return(e);
                se.open = 0;
                continue;
            }
            for (u32 i = se.body_start; i < se.body_end && !c->failed; i++) {
                const Sm70Insn* in = &insn[i];
                int always = in->pred == SM70_PT && !in->pred_not, never = in->pred == SM70_PT && in->pred_not;
                if (never) continue;
                if (in->op == SM70_BRX) { /* the block's end: SHS_SWITCH takes the target's instruction */
                    c->branch_target = spv_emit_shift_right_u32(
                        e, c->t_u32,
                        spv_emit_iadd_u32(e, c->t_u32, rd(c, sm70_field(in, 24, 8)), cu(c, brx_bias(in, i))), cu(c, 4));
                    continue;
                }
                if (in->op == SM70_BRA) { /* the block's end: SHS_IF takes its condition */
                    c->branch_cond = always ? 0 : prd(c, in->pred, in->pred_not, 0);
                    continue;
                }
                if (in->op == SM70_EXIT && always) {
                    epilogue(c);
                    spv_emit_return(e);
                    se.open = 0;
                    break;
                }
                u32 merge = 0;
                if (!always) {
                    u32 cond = prd(c, in->pred, in->pred_not, 0), then = spv_emit_id(e);
                    merge = spv_emit_id(e);
                    spv_emit_selection_merge(e, merge);
                    spv_emit_branch_conditional(e, cond, then, merge);
                    spv_emit_label(e, then);
                }
                if (in->op == SM70_EXIT) {
                    epilogue(c);
                    spv_emit_return(e);
                } else {
                    c->cur = i;
                    emit_insn(c, in);
                    if (merge) spv_emit_branch(e, merge);
                }
                if (merge) spv_emit_label(e, merge);
            }
        }
        if (se.open && !c->failed) {
            epilogue(c);
            spv_emit_return(e);
        }
        spv_emit_function_end(e);
        free(se.label);
        if (!c->failed) {
            spv_emit_entry_str(e, (SpvShaderStage)stage, fn, "main", c->interfaces, c->ninterfaces);
            if (stage == ST_FS) spv_emit_fragment_origin_upper_left(e, fn);
            if (stage == ST_FS && c->depth) spv_emit_fragment_depth_replacing(e, fn);
            if (stage == ST_CS) {
                u32 x = si->cs_local[0] ? si->cs_local[0] : 1, y = si->cs_local[1] ? si->cs_local[1] : 1,
                    z = si->cs_local[2] ? si->cs_local[2] : 1;
                spv_emit_local_size(e, fn, x, y, z);
            }
            /* Maxwell's and Ampere's programs use infinities and NaNs (CLAUDE.md) */
            spv_emit_float_controls(e, fn, 0, 1, 0);
            out = spv_emit_finish(e, word_count);
        }
        spv_emit_destroy(e);
    }
    if (!out && si->unsupported) sm70_debug_dump("program", code, bytes, sph, si->first_unsupported);
    free(st);
    free(c->addr_key);
    free(insn);
    free(words);
    free(c);
    return out;
}
