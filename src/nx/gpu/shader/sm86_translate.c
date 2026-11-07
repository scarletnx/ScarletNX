/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The operand order of the texture instructions is that of Mesa's NAK (src/nouveau/compiler/nak_nir_lower_tex.c):
   Copyright © 2023 Collabora, Ltd.
   SPDX-License-Identifier: MIT */
/* SM86 programs to SPIR-V (sm86.h). The program's registers, uniform registers and predicates are Function
   variables; constant buffers, global buffers, textures and attributes are the resources of the Maxwell translator's
   layout (shader.h), declared when an instruction first uses them, so that the renderer binds both alike.

   Control flow is the program's own branches. BSSY, BSYNC, BREAK and WARPSYNC only manage the convergence of the
   warp, so the translation reads past them, and structure.c (shader_structure_graph) places the blocks that BRA and
   EXIT end into structured constructs. structure_step follows spirv_translate.c's, which takes its own context. */
#include <stddef.h>
#include "nx/gpu/shader/sm86.h"
#include "nx/gpu/shader/translator.h"
#include "common/utilities/spirv_emit.h"
#include <stdarg.h>

#define MAX_INTERFACES 256

/* What prepare_handles found, by instruction. A TEX.B: its slot + 1, and for an indexed handle its LDC + 1. That LDC:
   the variable that keeps the element it picks, the table's first byte, the bytes between its elements */
typedef struct {
    u32 slot, ldc, var, first, stride;
} TexHandle;
typedef struct {
    SpvEmitter e;
    ShaderInfo* si;
    int stage;
    const u32* sph;
    const Sm86Insn* in;
    u32 n;
    /* types and constants */
    u32 t_void, t_bool, t_u32, t_i32, t_f32, t_v2f, t_v3f, t_v4f, t_v4u, t_v3u, t_v2u;
    u32 t_u64_; /* made with the Int64 capability when an instruction needs it (u64_type) */
    u32 zero, true_v, false_v, glsl;
    /* the program's state: Function variables, made at the start */
    u32 reg[256], ureg[64], pred[8], upred[8];
    /* resources, made when first used */
    u32 cbuf[18], cbuf_ptr, root;
    u32 gbuf[16], gbuf_ptr, smap;
    u32 wpages, mbase; /* SNX_VKB_WPAGES and SNX_VKB_MBASE (the GPU mirror's; the write log's bases are in gbuf_mark) */
    u32 gkey;   /* the global buffer the last global_address named */
    u32 shared, shared_ptr, shared_words;
    u32 local, local_ptr, local_words; /* LDL and STL's memory: a Function array, made at the start */
    u32 in_attr[32], out_attr[32], position, point_size, fragcoord, color[8], depth;
    u32 vertex_index, instance_index, local_id, group_id, lane_id;
    u32 push, push_ptr; /* a vertex program's push constant vec4: the base instance, the depth fix (vk/render.c) */
    u32 centroid_mask;  /* the inputs decorated Centroid */
    u32 legacy_fs;      /* a fragment program's legacy varying groups (shader_legacy_group), read as generics 17-31 */
    u32 point_coord, front_facing;
    u32 tex[SH_MAXTEX], tex_sampled[SH_MAXTEX];
    /* an indexed slot's arrays on SNX_VKB_TEXARRAY and SNX_VKB_TEXDEPTH, by kind (its tex[] is the element of the
       instruction being written), and SNX_VKB_TEXMAP and SNX_VKB_TEXFUNCS */
    u32 tex_array[SH_MAXTEX], tex_depth[SH_MAXTEX], tex_depth_type[SH_MAXTEX];
    u32 tex_map, tex_map_ptr, tex_func, tex_func_ptr;
    TexHandle* handle; /* by instruction, what prepare_handles found: NULL for a program without TEX.B */
    u32 img[8], img_type[8];
    u32 interfaces[MAX_INTERFACES], ninterfaces;
    /* the uniform register pair a ULDC.64 last loaded from a constant buffer: (bank << 16 | offset) + 1, 0 for none */
    u32* addr_key;   /* by instruction: the constant buffer word (bank << 16 | offset) + 1 of an LDG's or STG's base */
    u32* jump_first; /* by instruction, the first of the BRAs and BRX that go to it in jump_from (n + 1 words), and */
    u32* jump_from;  /* those instructions: made the first time reg_const_at needs them (flow_sources) */
    u32 cur;         /* the instruction being written */
    u32 branch_cond; /* the condition of the BRA that ends the body before, 0 for always */
    u32 branch_target; /* the instruction that the BRX which ends the body before goes to */
    /* a task or mesh program's ISBE (isbe_var): a mesh program's attribute area and map (Workgroup), a task
       payload (TaskPayloadWorkgroupEXT), their words, and LocalInvocationIndex */
    u32 isbe_attr, isbe_map, payload, isbe_attr_words, isbe_map_words, payload_words, local_index;
    u32 vertex_words, primitive_words; /* the output words of a vertex and of a primitive (omap_words) */
    u32 prim_attr[32];                 /* a mesh program's per-primitive generic outputs */
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

static u32 rd(Ctx* c, u32 r) { return r >= SM86_RZ ? c->zero : spv_emit_load(&c->e, c->t_u32, c->reg[r]); }
static void wr(Ctx* c, u32 r, u32 v) {
    if (r < SM86_RZ) spv_emit_store(&c->e, c->reg[r], v);
}
static u32 urd(Ctx* c, u32 r) { return r >= SM86_URZ ? c->zero : spv_emit_load(&c->e, c->t_u32, c->ureg[r]); }
static void uwr(Ctx* c, u32 r, u32 v) {
    if (r < SM86_URZ) { spv_emit_store(&c->e, c->ureg[r], v); }
}
static u32 prd(Ctx* c, u32 p, u32 negate, int uniform) {
    u32 v = p >= SM86_PT ? c->true_v : spv_emit_load(&c->e, c->t_bool, uniform ? c->upred[p] : c->pred[p]);
    return negate ? spv_emit_bool_not(&c->e, c->t_bool, v) : v;
}
static void pwr(Ctx* c, u32 p, u32 v, int uniform) {
    if (p < SM86_PT) spv_emit_store(&c->e, uniform ? c->upred[p] : c->pred[p], v);
}
/* the destination of an ALU instruction, a register or a uniform register */
static void dwr(Ctx* c, const Sm86Insn* in, u32 v) {
    if (sm86_ops[in->op].kind == SM86_K_UALU)
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
/* the root tables, banks 24 to 31, as one uniform buffer of SH_ROOT_BYTES (SNX_VKB_ROOT), with end the bytes read */
static u32 root_var(Ctx* c, u32 end) {
    if (!c->root) {
        c->root = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                              SNX_VKB_ROOT, SH_ROOT_BYTES / 16, &c->cbuf_ptr);
        add_interface(c, c->root);
    }
    if (end > c->si->root_max) c->si->root_max = end > SH_ROOT_BYTES ? SH_ROOT_BYTES : end;
    return c->root;
}
/* the word at byte offset off of a bank, plus the register value index when it isn't 0. A root table's bytes are
   past those of the tables before it, and an index stays in the tables */
static u32 cbuf_load(Ctx* c, u32 bank, u32 off, u32 index) {
    u32 root = bank >= SH_ROOT_BANK && bank < SH_ROOT_BANK + SH_ROOT_TABLES;
    u32 elements = root ? SH_ROOT_BYTES / 16 : 4096;
    if (root) off += (bank - SH_ROOT_BANK) * SH_ROOT_TABLE;
    u32 var = root ? root_var(c, index ? SH_ROOT_BYTES : off + 4) : cbuf_var(c, bank, index ? 0x10000 : off + 4);
    if (!var) return c->zero;
    u32 element, component;
    if (index) {
        u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off));
        element = spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 4)),
                                           cu(c, elements - 1));
        component = spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 2)),
                                             cu(c, 3));
    } else {
        element = cu(c, (off >> 4) & (elements - 1));
        component = cu(c, (off >> 2) & 3);
    }
    return spv_emit_uniform_component(&c->e, c->cbuf_ptr, var, c->zero, element, component, c->t_u32);
}

/* ---- ALU sources ---- */

static u32 src_bits(Ctx* c, const Sm86Src* s) {
    switch (s->kind) {
        case SM86_SRC_REG: return rd(c, s->reg);
        case SM86_SRC_UREG: return urd(c, s->reg);
        case SM86_SRC_IMM: return cu(c, s->value);
        case SM86_SRC_CBUF:
            if (s->bindless) {
                fail(c, "bindless constant buffer operand");
                return c->zero;
            }
            return cbuf_load(c, s->bank, s->value, 0);
        default: return c->zero;
    }
}
/* an integer source with its negation */
static u32 src_int(Ctx* c, const Sm86Src* s) {
    u32 v = src_bits(c, s);
    return s->neg && s->kind != SM86_SRC_IMM ? spv_emit_isub_u32(&c->e, c->t_u32, c->zero, v) : v;
}
/* a float source with its absolute value and negation */
static u32 src_f(Ctx* c, const Sm86Src* s) {
    u32 v = to_f(c, src_bits(c, s));
    if (s->kind == SM86_SRC_IMM) return v;
    if (s->abs) v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, v);
    if (s->neg) v = spv_emit_fnegate(&c->e, c->t_f32, v);
    return v;
}
/* the second source of a two-source ALU instruction: b, or c for the forms that put a register in b's place and the
   operand in c's (FADD's register forms keep it in b) */
static const Sm86Src* second(const Sm86Insn* in) {
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
static u32 carry_in(Ctx* c, u32 field_lo, const Sm86Insn* in, int uniform) {
    u32 p = prd(c, sm86_field(in, field_lo, 3), sm86_field(in, field_lo + 3, 1), uniform);
    return spv_emit_select(&c->e, u64_type(c), p, c64(c, 1), c64(c, 0));
}
/* a source of a 64-bit sum: its two's complement as NAK's fold reads it, ~x + 1 in 33 bits */
static u32 term64(Ctx* c, const Sm86Src* s) {
    u32 v = src_bits(c, s);
    if (!s->neg || s->kind == SM86_SRC_IMM) return to64(c, v);
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
/* d rounded to nearest as a float, and that float back as a double, both exact (NoContraction): NIR takes a double
   rounded to a float and widened again for the double itself where the conversions may be inexact, and the round trip
   then never differs from d, which hid every step of a directed rounding (RADV, lavapipe) */
static u32 to_float_and_back(Ctx* c, u32 d, u32* back) {
    u32 r = nocontract(c, SPV(&c->e, SPV_OP_F_CONVERT, c->t_f32, d));
    *back = nocontract(c, widen(c, r));
    return r;
}
/* a double rounded to a float by mode: exact when the double holds the exact result */
static u32 round_f64(Ctx* c, u32 mode, u32 d) {
    if (mode == 0) return SPV(&c->e, SPV_OP_F_CONVERT, c->t_f32, d);
    u32 back, r = to_float_and_back(c, d, &back);
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
    u32 back, r = to_float_and_back(c, sum, &back);
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
       used component's, and perspective for none, as spirv_translate.c reads Maxwell's (the same layout). A legacy
       varying's generic takes the color map (word 14) or the texture coordinate map (bytes 0x3c-0x45) of its group,
       as to_spirv gives Maxwell's */
    u32 g = input - LEGACY_GENERIC, map = 0;
    if (input >= LEGACY_GENERIC && (c->legacy_fs >> g & 1)) {
        if (g < 2) map = c->sph[14] >> (8 * g) & 0xff;
        if (g >= 5) map = c->sph[15 + (g - 5) / 4] >> ((g - 5) % 4 * 8) & 0xff;
    }
    for (u32 k = 0; k < 4; k++) {
        u32 word =
            input >= LEGACY_GENERIC && (c->legacy_fs >> g & 1) ? map : c->sph[6 + input / 4] >> ((input % 4) * 8);
        u32 mode = (word >> (k * 2)) & 3;
        if (mode) return mode;
    }
    return 2;
}
static void mesh_capability(Ctx* c) {
    spv_emit_capability(&c->e, 5283 /* MeshShadingEXT */);
    spv_emit_extension(&c->e, "SPV_EXT_mesh_shader");
}
static u32 input_var(Ctx* c, u32 i) {
    if (!c->in_attr[i]) {
        int interpolation = 0;
        int per_primitive = c->stage == ST_FS && (c->si->prim_mask >> i & 1);
        if (c->stage == ST_FS) {
            u32 mode = interp_mode(c, i);
            interpolation = per_primitive ? 1 : (mode == 1 ? 1 : mode == 3 ? 2 : 0); /* a per-primitive input is Flat */
        }
        u32 type = c->t_v4f;
        if (c->stage == ST_VS && (c->si->attr_int_mask & (1u << i)))
            type = c->si->attr_sint_mask & (1u << i) ? spv_emit_type_vec4(&c->e, c->t_i32) : c->t_v4u;
        c->in_attr[i] = spv_emit_input_vec4(&c->e, type, i, interpolation);
        add_interface(c, c->in_attr[i]);
        if (per_primitive) { /* a mesh program's output per primitive */
            mesh_capability(c);
            spv_emit_decorate(&c->e, c->in_attr[i], 5271 /* PerPrimitiveEXT */, NULL, 0);
        }
        c->si->in_mask |= 1u << i;
    }
    return c->in_attr[i];
}
/* the bits of component k of generic input i. A fragment program's input that no output of the stage before writes
   (in_unfed_mask) reads 0, 0, 0, 1, as the Maxwell translator reads it, with no Input for the stage before to match */
static u32 input_load(Ctx* c, u32 i, u32 k) {
    if (c->stage == ST_FS && (c->si->in_unfed_mask >> i & 1)) {
        c->si->in_mask |= 1u << i;
        return to_u(c, cf(c, k == 3 ? 1.0f : 0.0f));
    }
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
/* the vertex output a generic or position address names (a legacy varying's generic), made on its first use: 0 for
   another address */
static u32 output_var(Ctx* c, u32 addr) {
    if (shader_legacy_group(addr) >= 0) addr = shader_legacy_address(addr);
    if (addr >= 0x70 && addr < 0x80) {
        position_var(c);
        return c->position;
    }
    if (addr < 0x80 || addr >= 0x280) return 0;
    u32 i = (addr - 0x80) >> 4;
    if (!c->out_attr[i]) {
        c->out_attr[i] = spv_emit_output_vec4(&c->e, c->t_v4f, i);
        add_interface(c, c->out_attr[i]);
        c->si->out_mask |= 1u << i;
    }
    return c->out_attr[i];
}
static void attr_store(Ctx* c, u32 addr, u32 bits) {
    if (shader_legacy_group(addr) >= 0) addr = shader_legacy_address(addr);
    u32 var = 0, k = (addr >> 2) & 3;
    if (c->stage == ST_FS || c->stage == ST_CS) {
        fail(c, "attribute store in a fragment or compute program");
        return;
    }
    if (c->stage != ST_VS) {
        fail(c, "attribute store of stage %d", c->stage);
        return;
    }
    if (addr == 0x5c) {
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
    }
    var = output_var(c, addr);
    if (!var) {
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
        /* global memory as the console has it: two buffers may be the same bytes (one game binds a range and a part of
           it, writes through one and reads through the other), and another invocation's store is seen. Without these
           the compiler kept a value it had read, and a program waiting on its own store through the other buffer
           never finished: the GPU's context timed out */
        spv_emit_decorate(&c->e, c->gbuf[slot], 20 /* Aliased */, NULL, 0);
        spv_emit_decorate(&c->e, c->gbuf[slot], 23 /* Coherent */, NULL, 0);
        add_interface(c, c->gbuf[slot]);
    }
    if (write) c->si->gbuf_wmask |= 1u << slot;
    else
        c->si->gbuf_rmask |= 1u << slot;
    return c->gbuf[slot];
}
/* a compute program's atomic reads the global buffer it operates on: mark it read, as a load does (gbuf_slot only sees
   the write side of an atomic) */
static void mark_read(Ctx* c, u32 key) {
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) {
            c->si->gbuf_rmask |= 1u << i;
            return;
        }
}
/* A compute program's store of bytes bytes (1, 2, 4, 8 or 16) at byte offset into global buffer key records them in
   the write log (ShaderInfo.gbuf_mark, SH_WLOG_*): a plain store of 1 into its window's flag, as every store of a
   window writes the same, and an atomic OR of its bytes' bits into that window's mask, which other invocations OR
   into too. The offset is taken as aligned to the size, as the GPU requires of every global access, so its bits are
   in one mask word. A store past the windows sets the last window's flag and no bit. The renderer lands the bytes
   logged and no others: an Ounce program's buffer is all of the memory mapped from its address, up to 16 MiB, and
   finding what changed by comparing its bytes with a copy taken before the dispatch was most of one game's CPU time */
static void mark_written(Ctx* c, u32 key, u32 offset, u32 bytes) {
    int slot = -1;
    if (c->stage != ST_CS) return;
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) slot = i;
    if (slot < 0) return;
    if (!c->si->gbuf_mark[slot]) c->si->gbuf_mark[slot] = ++c->si->nmarks;
    u32 part = (u32)(c->si->gbuf_mark[slot] - 1) * SH_WLOG_PART;
    if (!c->wpages) {
        u32 array = spv_emit_type_runtime_array(&c->e, c->t_u32, 4);
        u32 structure = spv_emit_type_struct_one(&c->e, array);
        c->wpages = spv_emit_storage_buffer(&c->e, structure, (u32)shader_vk_set(c->stage), SNX_VKB_WPAGES);
        add_interface(c, c->wpages);
    }
    u32 ptr_type = spv_emit_type_storage_pointer(&c->e, c->t_u32);
    u32 at = spv_emit_bitwise_and_u32(&c->e, c->t_u32, offset, cu(c, ~(bytes - 1)));
    u32 window = spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 12)), last = cu(c, SH_WPAGE_SLOT - 1);
    u32 past = spv_emit_u_less(&c->e, c->t_bool, last, window);
    window = spv_emit_select(&c->e, c->t_u32, past, last, window);
    u32 flag = spv_emit_iadd_u32(&c->e, c->t_u32, window, cu(c, part));
    spv_emit_store(&c->e, spv_emit_storage_access_u32(&c->e, ptr_type, c->wpages, c->zero, flag), cu(c, 1));
    u32 word = spv_emit_iadd_u32(
        &c->e, c->t_u32,
        spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_imul_u32(&c->e, c->t_u32, window, cu(c, SH_WLOG_MASK)),
                          spv_emit_shift_right_u32(
                              &c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, at, cu(c, 4095)), cu(c, 5))),
        cu(c, part + SH_WPAGE_SLOT));
    u32 bits = spv_emit_shift_left_u32(&c->e, c->t_u32, cu(c, (u32)((1ull << bytes) - 1)),
                                       spv_emit_bitwise_and_u32(&c->e, c->t_u32, at, cu(c, 31)));
    bits = spv_emit_select(&c->e, c->t_u32, past, cu(c, 0), bits);
    spv_emit_atomic_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_OR,
                        spv_emit_storage_access_u32(&c->e, ptr_type, c->wpages, c->zero, word), bits);
    c->si->gbuf_marks |= 1u << slot;
}
/* LDG and STG, as spirv_translate.c's global memory: the buffer is the one whose 64-bit address the constant buffer
   word that track_globals found holds (ShaderInfo.gbuf_cbuf_off), and the byte offset into it is the address less
   that word, read at run time. The address is a register (a pair with bit 90 for the uniform form, bit 72 for the
   other) and a uniform register pair (bit 72), with a signed 24-bit offset; the low 32 bits of the difference serve */
static u32 global_address(Ctx* c, const Sm86Insn* in, int uniform_form, int write, u32* offset) {
    u32 key = c->addr_key[c->cur];
    if (!key) {
        u32 src = c->addr_key[c->n + 1 + c->cur];
        fail(c, "global address whose base no constant buffer holds (from %s, at %u)",
             src ? sm86_ops[src].name : "nothing", c->cur);
        return 0;
    }
    key--;
    u32 imm = sm86_field(in, 40, 24), reg = sm86_field(in, 24, 8);
    u32 bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, rd(c, reg), cu(c, bias));
    if (uniform_form) {
        u32 ur = sm86_field(in, in->op == SM86_LDG ? 32 : 64, 6);
        if (ur < SM86_URZ) at = spv_emit_iadd_u32(&c->e, c->t_u32, at, urd(c, ur));
    }
    if (SH_GBUF_BANK(key) >= 18) { /* the renderer reads the base from a bound bank (vk/render.c) */
        fail(c, "global address whose base is in constant buffer %u", SH_GBUF_BANK(key));
        return 0;
    }
    u32 base = cbuf_load(c, SH_GBUF_BANK(key), SH_GBUF_OFFSET(key), 0);
    *offset = spv_emit_isub_u32(&c->e, c->t_u32, at, base);
    c->gkey = key;
    return gbuf_slot(c, key, write);
}
/* An LDG or STG whose address is a pointer that the program loaded from memory, which no constant buffer word holds, as one
   game's vertex program that clears the indirect draws a table of pointers names. As the Maxwell translator does
   (shader_global_dynamic, after Eden's DefineGlobalMemoryFunctions), its 64-bit address is compared with each global
   buffer of the program when it runs, and the first that holds it takes the access. A buffer is the 16 MiB that the
   renderer binds from the address its word holds (vk_gbuf_desc). A load from none of them reads 0 and a store there
   is dropped, where the console's GPU reaches all of its address space. The address: a register, a pair with bit 90
   in the uniform form or bit 72 in the other, plus the uniform register (a pair with bit 72) of the uniform form,
   plus the signed 24-bit offset */
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst);
static void mem_store(Ctx* c, u32 buffer, u32 address, u32 type, u32 data);
static void global_dynamic(Ctx* c, const Sm86Insn* in, int uniform_form, int load) {
    u32 type = sm86_field(in, 73, 3), words = type == 5 ? 2 : type == 6 ? 4 : 1;
    u32 reg = sm86_field(in, 24, 8), imm = sm86_field(in, 40, 24);
    u32 hi = sm86_field(in, uniform_form ? 90 : 72, 1) && reg < SM86_RZ ? rd(c, reg + 1) : c->zero;
    u32 address = pair64(c, rd(c, reg), hi);
    u64 bias = imm & 0x800000u ? (u64)imm | 0xffffffffff000000ull : imm;
    address = op2(c, SPV_OP_I_ADD, u64_type(c), address, c64(c, bias));
    if (uniform_form) {
        u32 ur = sm86_field(in, in->op == SM86_LDG ? 32 : 64, 6);
        if (ur < SM86_URZ) {
            u32 uhi = sm86_field(in, 72, 1) && ur + 1 < SM86_URZ ? urd(c, ur + 1) : c->zero;
            address = op2(c, SPV_OP_I_ADD, u64_type(c), address, pair64(c, urd(c, ur), uhi));
        }
    }
    if (load)
        for (u32 k = 0; k < words && in->dst + k < SM86_RZ; k++) wr(c, in->dst + k, c->zero);
    /* the program's buffers, each key once, in the order of their first access */
    u32 keys[16], nkeys = 0;
    for (u32 i = 0; i < c->n; i++) {
        u32 key = c->addr_key[i], seen = 0;
        if (!key || SH_GBUF_BANK(key - 1) >= 18) continue;
        for (u32 k = 0; k < nkeys; k++) seen |= keys[k] == key - 1;
        if (!seen && nkeys < 16) keys[nkeys++] = key - 1;
    }
    u32 merges[16];
    for (u32 k = 0; k < nkeys; k++) {
        u32 buffer = gbuf_slot(c, keys[k], !load);
        if (!buffer) return;
        u32 base = pair64(c, cbuf_load(c, SH_GBUF_BANK(keys[k]), SH_GBUF_OFFSET(keys[k]), 0),
                          cbuf_load(c, SH_GBUF_BANK(keys[k]), SH_GBUF_OFFSET(keys[k]) + 4, 0));
        u32 relative = op2(c, SPV_OP_I_SUB, u64_type(c), address, base);
        u32 inside = spv_emit_u_less(&c->e, c->t_bool, relative, c64(c, 16u << 20));
        u32 then = spv_emit_id(&c->e), other = spv_emit_id(&c->e);
        merges[k] = spv_emit_id(&c->e);
        spv_emit_selection_merge(&c->e, merges[k]);
        spv_emit_branch_conditional(&c->e, inside, then, other);
        spv_emit_label(&c->e, then);
        u32 offset = lo32(c, relative);
        if (load)
            mem_load(c, buffer, offset, type, in->dst, 0);
        else
            mem_store(c, buffer, offset, type, sm86_field(in, 32, 8));
        spv_emit_branch(&c->e, merges[k]);
        spv_emit_label(&c->e, other);
    }
    for (u32 k = nkeys; k-- > 0;) {
        spv_emit_branch(&c->e, merges[k]);
        spv_emit_label(&c->e, merges[k]);
    }
}
/* what track_globals follows at an instruction: each register's key, then the opcode that last wrote it (for the
   message of an address without one). The keys first: the flow settles on them */
typedef struct {
    u32 rkey[256], ukey[64];
    u16 rsrc[256], usrc[64];
} TrackState;
#define TRACK_KEYS offsetof(TrackState, rsrc)
/* one instruction of track_globals: an LDG's or STG's key into addr_key, and what its destination gets */
static void track_step(Ctx* c, u32 i, TrackState* s) {
    const Sm86Insn* in = &c->in[i];
    int op = in->op, uniform = sm86_ops[op].kind == SM86_K_UALU;
    if (op == SM86_LDG || op == SM86_STG || op == SM86_LDG_NOU || op == SM86_STG_NOU || op == SM86_ATOMG ||
        op == SM86_ATOMG_NOU || op == SM86_RED) {
        int uniform_form = op == SM86_LDG || op == SM86_STG || op == SM86_ATOMG || op == SM86_RED;
        u32 reg = sm86_field(in, 24, 8), ur = uniform_form ? sm86_field(in, op == SM86_LDG ? 32 : 64, 6) : SM86_URZ;
        u32 k = ur < SM86_URZ && s->ukey[ur] ? s->ukey[ur] : reg < SM86_RZ ? s->rkey[reg] : 0;
        c->addr_key[i] = k;
        c->addr_key[c->n + 1 + i] = ur < SM86_URZ && s->ukey[ur] ? s->usrc[ur] : reg < SM86_RZ ? s->rsrc[reg] : 0;
        if (!k && ur < SM86_URZ && !s->ukey[ur] && s->usrc[ur]) c->addr_key[c->n + 1 + i] = s->usrc[ur];
        if (op == SM86_LDG || op == SM86_LDG_NOU || op == SM86_ATOMG || op == SM86_ATOMG_NOU) {
            /* the words loaded are data: one, or two or four for an LDG.64 or .128 */
            u32 type = sm86_field(in, 73, 3), words = op == SM86_ATOMG || op == SM86_ATOMG_NOU ? 1
                                                      : type == 5                              ? 2
                                                      : type == 6                              ? 4
                                                                                               : 1;
            for (u32 w = 0; w < words && in->dst + w < SM86_RZ; w++)
                s->rkey[in->dst + w] = 0, s->rsrc[in->dst + w] = (u16)op;
        }
        return;
    }
    u32 key = 0;
    int found = 0;
    if (op == SM86_ULDC || (op == SM86_LDC && sm86_field(in, 24, 8) == SM86_RZ))
        key = (sm86_field(in, 54, 5) << 16 | sm86_field(in, 38, 16)) + 1, found = 1;
    else if (op == SM86_MOV || op == SM86_UMOV || op == SM86_R2UR) {
        const Sm86Src* b = &in->b;
        if (op == SM86_R2UR)
            key = s->rkey[sm86_field(in, 24, 8) & 255];
        else if (b->kind == SM86_SRC_CBUF && !b->bindless)
            key = (u32)(b->bank << 16 | b->value) + 1;
        else if (b->kind == SM86_SRC_REG && b->reg < SM86_RZ)
            key = s->rkey[b->reg];
        else if (b->kind == SM86_SRC_UREG && b->reg < SM86_URZ)
            key = s->ukey[b->reg];
        found = 1;
    } else if ((op == SM86_IADD3 || op == SM86_UIADD3 || op == SM86_IMAD || op == SM86_UIMAD ||
                op == SM86_IMAD_WIDE || op == SM86_UIMAD_WIDE || op == SM86_LEA || op == SM86_ULEA) &&
               !sm86_field(in, 74, 1) && !((op == SM86_LEA || op == SM86_ULEA) && sm86_field(in, 80, 1))) {
        /* one source with a base: the result is that base at an offset. A constant buffer operand is a base
           itself, as in ptxas's LEA R0, P0, R2, c[0x0][0x60], 0x2 */
        const Sm86Src* s3[3] = {&in->a, &in->b, &in->c};
        u32 count = 0, first = 0, last = 3;
        if (op == SM86_IMAD || op == SM86_UIMAD || op == SM86_IMAD_WIDE || op == SM86_UIMAD_WIDE)
            first = 2; /* a product is an offset: only the sum's c can be the base */
        else if (op == SM86_LEA || op == SM86_ULEA)
            first = 1, last = 2; /* the shifted a is an offset: only b */
        for (u32 k = first; k < last; k++) {
            const Sm86Src* x = s3[k];
            u32 v = x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rkey[x->reg]
                    : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->ukey[x->reg]
                    : x->kind == SM86_SRC_CBUF && !x->bindless      ? (u32)(x->bank << 16 | x->value) + 1
                                                                    : 0;
            if (v) key = v, count++;
        }
        if (count != 1) key = 0;
        found = 1;
    }
    /* the destination: the key, or none for any other write (the high word of a pair takes none either) */
    u32 d = uniform || op == SM86_ULDC || op == SM86_UMOV || op == SM86_R2UR ? sm86_field(in, 16, 6) : in->dst;
    int to_uniform = uniform || op == SM86_ULDC || op == SM86_R2UR || op == SM86_S2UR || op == SM86_ULDC_IDX;
    int wide = op == SM86_ULDC || op == SM86_LDC || op == SM86_IMAD_WIDE || op == SM86_UIMAD_WIDE;
    if (!found) key = 0;
    if (to_uniform) {
        if (d < SM86_URZ) s->ukey[d] = key, s->usrc[d] = (u16)op;
        if (wide && d + 1 < SM86_URZ) s->ukey[d + 1] = 0;
    } else if ((sm86_ops[op].kind == SM86_K_ALU || op == SM86_LDC || op == SM86_S2R || op == SM86_CS2R) &&
               op != SM86_FSETP && op != SM86_ISETP && op != SM86_DSETP && op != SM86_HSETP2 && op != SM86_R2P) {
        if (d < SM86_RZ) s->rkey[d] = key, s->rsrc[d] = (u16)op;
        if (wide && d + 1 < SM86_RZ) s->rkey[d + 1] = 0;
    }
}
/* BRX, and BRXU, which takes its index from a uniform register */
static int is_brx(int op) { return op == SM86_BRX || op == SM86_BRXU; }
static int falls_through(const Sm86Insn* in);
static int flow_sources(Ctx* c);
/* The constant buffer word that holds the base of each LDG's and STG's address: a register gets the word that
   ULDC.64, LDC.64 or MOV loads into it, and keeps it through moves and through IADD3, IMAD, IMAD.WIDE and LEA with
   one source that has it, as Eden's Track follows Maxwell's. Along the flow: a block starts with the keys its
   reached predecessors end with where they agree, until no block's end changes. In program order, a jump table's
   cases wrote over a register that the code after them had from before them, on every path */
static int track_globals(Ctx* c) {
    u32 n = c->n, nb = 0;
    c->addr_key = calloc(2 * ((size_t)n + 1), sizeof *c->addr_key);
    if (!c->addr_key || !flow_sources(c)) return 0;
    if (c->failed) return 1;
    u32* start = malloc(((size_t)n + 1) * sizeof *start);
    u32* block_of = malloc(((size_t)n + 1) * sizeof *block_of);
    TrackState *out = NULL, *s = malloc(sizeof *s);
    u8* reached = NULL;
    int ok = start && block_of && s;
    for (u32 i = 0; ok && i < n; i++) {
        const Sm86Insn* prev = i ? &c->in[i - 1] : NULL;
        if (!i || c->jump_first[i] < c->jump_first[i + 1] || !falls_through(prev) || prev->op == SM86_BRA ||
            is_brx(prev->op))
            start[nb++] = i;
        block_of[i] = nb - 1;
    }
    if (ok) {
        start[nb] = n;
        out = malloc((size_t)nb * sizeof *out);
        reached = calloc(nb, 1);
        ok = out && reached;
    }
    /* the keys only fall to none, but where one source of an add has a base: a loop could go round, so a cap */
    int changed = 1;
    for (u32 pass = 0; ok && changed; pass++) {
        if (pass == 64) {
            fail(c, "global address bases the flow doesn't settle");
            break;
        }
        changed = 0;
        for (u32 b = 0; b < nb; b++) {
            u32 first = start[b], preds = 0;
            if (!b) memset(s, 0, sizeof *s), preds = 1; /* the program's start: no register has a base */
            const u32 jumps = c->jump_first[first + 1] - c->jump_first[first];
            for (u32 k = 0; k <= jumps; k++) {
                u32 p;
                if (k == jumps) {
                    if (!first || !falls_through(&c->in[first - 1])) continue;
                    p = block_of[first - 1];
                } else
                    p = block_of[c->jump_from[c->jump_first[first] + k]];
                if (!reached[p]) continue;
                if (!preds++) {
                    *s = out[p];
                    continue;
                }
                for (u32 r = 0; r < 256; r++)
                    if (s->rkey[r] != out[p].rkey[r]) s->rkey[r] = 0;
                for (u32 r = 0; r < 64; r++)
                    if (s->ukey[r] != out[p].ukey[r]) s->ukey[r] = 0;
            }
            if (!preds) continue; /* nothing reaches it yet */
            for (u32 i = first; i < start[b + 1]; i++) track_step(c, i, s);
            if (!reached[b] || memcmp(&out[b], s, TRACK_KEYS)) changed = 1;
            out[b] = *s;
            reached[b] = 1;
        }
    }
    free(s);
    free(reached);
    free(out);
    free(block_of);
    free(start);
    return ok;
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
/* the buffer that names local memory to mem_ptr, mem_load and mem_store */
#define MEM_LOCAL 0xffffffffu
static u32 smap_word(Ctx* c, u32 index) {
    return spv_emit_load(&c->e, c->t_u32, spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, c->smap, c->zero, index));
}
/* Where a word of a global buffer is in what is bound. A draw whose writable storage shares a page with another of its
   buffers has them all bound as one atlas of pages (vk_buf_draw_prepare), and a table (SNX_VKB_SMAP) gives each
   buffer's pages, as shader_gbuf_word_index reads it for Maxwell's programs. One game's culling programs have their
   counters in the last page of their instances every other frame, and read unmapped there, whole objects came and went.
   A dispatch binds each buffer whole, and has no table */
static u32 gbuf_word_index(Ctx* c, u32 buffer, u32 word) {
    if (c->stage == ST_CS) return word;
    u32 slot = 0;
    while (slot < 15 && c->gbuf[slot] != buffer) slot++;
    if (!c->smap) {
        c->smap = spv_emit_storage_buffer(
            &c->e, spv_emit_type_struct_one(&c->e, spv_emit_type_runtime_array(&c->e, c->t_u32, 4)),
            (u32)shader_vk_set(c->stage), SNX_VKB_SMAP);
        add_interface(c, c->smap);
    }
    u32 first = smap_word(c, cu(c, slot)), offset = smap_word(c, cu(c, 16 + slot)),
        count = smap_word(c, cu(c, 32 + slot));
    u32 none = spv_emit_i_equal(&c->e, c->t_bool, first, cu(c, ~0u));
    u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, word, offset);
    u32 page = spv_emit_shift_right_u32(&c->e, c->t_u32, at, cu(c, 10));
    u32 entry = spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_select(&c->e, c->t_u32, none, c->zero, first),
                                  spv_emit_select(&c->e, c->t_u32, none, c->zero, page));
    u32 mapped = spv_emit_bitwise_or_u32(&c->e, c->t_u32,
                                         spv_emit_shift_left_u32(&c->e, c->t_u32, smap_word(c, entry), cu(c, 10)),
                                         spv_emit_bitwise_and_u32(&c->e, c->t_u32, at, cu(c, 1023)));
    mapped = spv_emit_select(&c->e, c->t_u32, spv_emit_u_less(&c->e, c->t_bool, word, count), mapped, cu(c, ~0u));
    return spv_emit_select(&c->e, c->t_u32, none, word, mapped);
}
/* a word of global (buffer), shared (buffer 0) or local (MEM_LOCAL) memory at the byte address. An address past
   the local memory wraps in it, as spirv_translate.c keeps Maxwell's LDL and STL in theirs */
static u32 mem_ptr(Ctx* c, u32 buffer, u32 address, u32 word) {
    u32 index =
        spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, address, cu(c, 2)), cu(c, word));
    if (buffer == MEM_LOCAL)
        return spv_emit_function_element_pointer(
            &c->e, c->local_ptr, c->local, spv_emit_bitwise_and_u32(&c->e, c->t_u32, index, cu(c, c->local_words - 1)));
    if (buffer)
        return spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, buffer, c->zero, gbuf_word_index(c, buffer, index));
    u32 last = cu(c, c->shared_words - 1);
    index = spv_emit_select(&c->e, c->t_u32, spv_emit_u_less(&c->e, c->t_bool, last, index), last, index);
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, c->shared_ptr, shared_var(c), index);
}
/* an address in shared or local memory: the register at 24 times 1, 4, 8 or 16 (bits 78-79, NAK's OffsetStride), the
   uniform register ur (SM86_URZ for none) and the signed 24-bit offset */
static u32 stack_address(Ctx* c, const Sm86Insn* in, u32 ur) {
    static const u8 shifts[4] = {0, 2, 3, 4};
    u32 imm = sm86_field(in, 40, 24);
    u32 bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    u32 base = rd(c, sm86_field(in, 24, 8)), shift = shifts[sm86_field(in, 78, 2)];
    if (shift) base = spv_emit_shift_left_u32(&c->e, c->t_u32, base, cu(c, shift));
    u32 address = spv_emit_iadd_u32(&c->e, c->t_u32, base, cu(c, bias));
    return ur < SM86_URZ ? spv_emit_iadd_u32(&c->e, c->t_u32, address, urd(c, ur)) : address;
}
/* An atomic's operation in SPIR-V, 0 for one it doesn't have: 87-90 the operation (0 add, 1 min, 2 max, 5 and, 6 or,
   7 xor, 8 exchange), 73-75 the type (0 u32, 1 s32) */
static u32 atomic_opcode(u32 type, u32 how) {
    static const u16 ops[2][9] = {{SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_U_MIN, SPV_OP_ATOMIC_U_MAX, 0, 0,
                                   SPV_OP_ATOMIC_AND, SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR, SPV_OP_ATOMIC_EXCHANGE},
                                  {SPV_OP_ATOMIC_I_ADD, SPV_OP_ATOMIC_S_MIN, SPV_OP_ATOMIC_S_MAX, 0, 0,
                                   SPV_OP_ATOMIC_AND, SPV_OP_ATOMIC_OR, SPV_OP_ATOMIC_XOR, SPV_OP_ATOMIC_EXCHANGE}};
    return type > 1 || how > 8 ? 0 : ops[type][how];
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
        if (dst >= SM86_RZ) break;
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
        if (buffer == MEM_LOCAL) /* the invocation's own: no other can store to the word */
            spv_emit_store(
                &c->e, ptr,
                spv_emit_bitwise_or_u32(&c->e, c->t_u32,
                                        spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_load(&c->e, c->t_u32, ptr),
                                                                 spv_emit_not_u32(&c->e, c->t_u32, mask)),
                                        bits));
        else if (buffer) {
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
        spv_emit_store(&c->e, mem_ptr(c, buffer, address, k), rd(c, data >= SM86_RZ ? data : data + k));
}

/* ---- task and mesh programs ---- */

/* A task or mesh program reads and writes its stage's memory, the ISBE ("internal stage buffer entry"), with ISBERD
   and ISBEWR at byte offsets (docs/platforms/nx/ounce.md, "Mesh shaders"). The translation keeps it in arrays of words
   that the program's own addressing indexes, so that no offset needs a decode:
   - a mesh program's attribute area (Workgroup): its per-vertex outputs, then its per-primitive outputs, in groups of
     32 (the SKEW form). Word k of vertex v is at byte (v / 32) * G + (v % 32) * 4 + k * 0x80, G 0x80 times the words
     of a vertex, in the order of the SPH's output map. The primitives' area starts after the vertex groups.
   - a mesh program's map (Workgroup), a byte to a word: the primitive count at byte 3 and the indices of primitive p
     at bytes 4 + 3 p on.
   - a task program's payload (TaskPayloadWorkgroupEXT): the count of mesh groups to launch at byte 4, and what its
     mesh programs read from byte 0x10 on, which a mesh program after a task program reads in the same array.
   The program's body is a function of its own (mesh_entry): after a workgroup barrier, main launches the mesh groups
   or writes the outputs from these arrays */
#define SM86_OMAP_FIRST    400u /* an SPH's output map (vertex, tessellation and geometry): bits 400 to 639 */
#define SM86_OMAP_END      640u
#define SM86_OMAP_POSITION 428u /* the position's x, y, z and w */
#define SM86_OMAP_GENERIC  432u /* component k of generic i at 432 + 4 i + k */
static int omap_bit(const u32* sph, u32 b) { return sph[b >> 5] >> (b & 31) & 1; }
/* the output words of an SPH's output map, in their ISBE order. Every bit must be a generic's, or the position's when
   position is set, else the program is refused: the facts of the game's programs give no other */
static u32 omap_words(Ctx* c, const u32* sph, int position, const char* what) {
    u32 n = 0;
    for (u32 b = SM86_OMAP_FIRST; b < SM86_OMAP_END; b++) {
        if (!omap_bit(sph, b)) continue;
        if (b >= SM86_OMAP_GENERIC + 128 || b < (position ? SM86_OMAP_POSITION : SM86_OMAP_GENERIC))
            fail(c, "a %s output of SPH output map bit %u", what, b);
        n++;
    }
    return n;
}
/* one of the ISBE arrays, made on its first use: words long in storage (4 Workgroup, 5402 TaskPayloadWorkgroupEXT) */
#define SPV_STORAGE_TASK_PAYLOAD 5402u
static u32 isbe_var(Ctx* c, u32* var, u32 storage, u32 words) {
    if (!*var) {
        *var = spv_emit_variable(&c->e, storage, spv_emit_type_array_u32(&c->e, c->t_u32, words));
        add_interface(c, *var);
    }
    return *var;
}
static u32 isbe_ptr(Ctx* c, u32 var, u32 storage, u32 index) {
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, spv_emit_type_pointer(&c->e, storage, c->t_u32), var, index);
}
/* word index of an ISBE array of words, or 0 past its end */
static u32 isbe_load(Ctx* c, u32 var, u32 storage, u32 words, u32 index) {
    u32 last = cu(c, words - 1);
    u32 inside = spv_emit_u_less(&c->e, c->t_bool, index, cu(c, words));
    u32 v = spv_emit_load(&c->e, c->t_u32,
                          isbe_ptr(c, var, storage, spv_emit_select(&c->e, c->t_u32, inside, index, last)));
    return spv_emit_select(&c->e, c->t_u32, inside, v, c->zero);
}
/* value into word index of an ISBE array of words: a store past its end goes nowhere */
static void isbe_store(Ctx* c, u32 var, u32 storage, u32 words, u32 index, u32 value) {
    u32 inside = spv_emit_u_less(&c->e, c->t_bool, index, cu(c, words));
    u32 then = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    spv_emit_selection_merge(&c->e, merge);
    spv_emit_branch_conditional(&c->e, inside, then, merge);
    spv_emit_label(&c->e, then);
    spv_emit_store(&c->e, isbe_ptr(c, var, storage, index), value);
    spv_emit_branch(&c->e, merge);
    spv_emit_label(&c->e, merge);
}
static u32 payload_var(Ctx* c) { return isbe_var(c, &c->payload, SPV_STORAGE_TASK_PAYLOAD, c->payload_words); }
static u32 map_var(Ctx* c) { return isbe_var(c, &c->isbe_map, SPV_STORAGE_WORKGROUP, c->isbe_map_words); }
/* The workgroup index that ISBERD.ATTR [RZ+0x0] reads: the vertex ID of the hardware's point draw for a task
   program, or a mesh program without one, which counts from the draw's first group (the push constant's x, as bits).
   A mesh program after a task program reads its index in its task's launch */
static u32 workgroup_index(Ctx* c) {
    u32 id = builtin_vec3(c, &c->group_id, 26, 0);
    if (c->stage == ST_MS && c->si->mesh.has_task) return id;
    return spv_emit_iadd_u32(&c->e, c->t_u32, id, to_u(c, push_component(c, 0)));
}
/* ISBERD and ISBEWR (NAK's sm70_encode.rs): the offset register at 24-31, the data register at 32-39 (ISBEWR), the
   immediate offset at 40-55, the size at 74-75 (U8 0, U16 1, B32 2), the access at 76-77 (MAP 0, PATCH 1, PRIM 2,
   ATTR 3), SKEW at 78 and the output bit at 79. Only the forms of the game's programs translate */
static void isbe(Ctx* c, const Sm86Insn* in) {
    int write = in->op == SM86_ISBEWR, task = c->stage == ST_TS, mesh = c->stage == ST_MS;
    u32 base = sm86_field(in, 24, 8), imm = sm86_field(in, 40, 16), size = sm86_field(in, 74, 2),
        access = sm86_field(in, 76, 2), skew = sm86_field(in, 78, 1), output = sm86_field(in, 79, 1);
    int fixed = base == SM86_RZ, b32 = size == 2, attr = access == 3;
    const ShMesh* m = &c->si->mesh;
    if (!task && !mesh) {
        fail(c, "%s outside a task or mesh program", sm86_ops[in->op].name);
        return;
    }
    if (b32 && fixed && (imm & 3) && !(write && access == 0 && imm == 3)) {
        fail(c, "%s.B32 at the byte offset 0x%x", sm86_ops[in->op].name, imm);
        return;
    }
    u32 address = fixed ? cu(c, imm) : spv_emit_iadd_u32(&c->e, c->t_u32, rd(c, base), cu(c, imm));
    u32 word = spv_emit_shift_right_u32(&c->e, c->t_u32, address, cu(c, 2));
    if (!write && attr && !output && b32 && fixed && !imm && skew == (u32)(task || !m->has_task)) {
        wr(c, in->dst, workgroup_index(c));
        return;
    }
    if (!write && mesh && m->has_task && attr && !skew && !output && b32) {
        wr(c, in->dst, isbe_load(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, c->payload_words, word));
        return;
    }
    u32 data = write ? rd(c, sm86_field(in, 32, 8)) : 0;
    if (write && task && attr && !skew && output && b32) {
        isbe_store(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, c->payload_words, word, data);
        return;
    }
    if (write && mesh && attr && skew && output && b32 && c->isbe_attr_words) {
        isbe_store(c, isbe_var(c, &c->isbe_attr, SPV_STORAGE_WORKGROUP, c->isbe_attr_words), SPV_STORAGE_WORKGROUP,
                   c->isbe_attr_words, word, data);
        return;
    }
    if (write && mesh && access == 0 && !skew && output && size == 0) {
        isbe_store(c, map_var(c), SPV_STORAGE_WORKGROUP, c->isbe_map_words, address,
                   spv_emit_bitwise_and_u32(&c->e, c->t_u32, data, cu(c, 0xff)));
        return;
    }
    if (write && mesh && access == 0 && !skew && output && b32 && fixed && imm == 3) {
        isbe_store(c, map_var(c), SPV_STORAGE_WORKGROUP, c->isbe_map_words, cu(c, 3), data);
        return;
    }
    static const char* const accesses[] = {"MAP", "PATCH", "PRIM", "ATTR"};
    static const char* const sizes[] = {"U8", "U16", "B32", "size 3"};
    char reg[8] = "RZ";
    if (!fixed) snprintf(reg, sizeof reg, "R%u", base);
    fail(c, "%s.%s%s%s.%s [%s+0x%x] in a %s program", sm86_ops[in->op].name, accesses[access], skew ? ".SKEW" : "",
         output ? ".O" : "", sizes[size], reg, imm, task ? "task" : "mesh");
}

/* ---- textures and surfaces ---- */

/* whether the instruction at k goes on to k + 1: not after a BRA, BRX or EXIT that always leaves, nor a RET */
static int falls_through(const Sm86Insn* in) {
    int always = in->pred == SM86_PT && !in->pred_not;
    return !((in->op == SM86_BRA || is_brx(in->op) || in->op == SM86_EXIT) && always) && in->op != SM86_RET;
}
static u32 brx_targets(Ctx* c, u32 i, u32* targets, u32 max);
/* the instructions that jump to each one, build_structure's edges: a BRA's target, a BRX's cases. Returns 0 when it
   can't be made */
static int flow_sources(Ctx* c) {
    if (c->jump_first) return 1;
    u32 n = c->n, total = 0, t[64];
    for (u32 i = 0; i < n; i++) {
        const Sm86Insn* in = &c->in[i];
        if (in->pred == SM86_PT && in->pred_not) continue;
        if (in->op == SM86_BRA) total++;
        else if (is_brx(in->op))
            total += brx_targets(c, i, t, 64);
    }
    u32* first = calloc((size_t)n + 1 + total + 1, sizeof *first);
    if (!first) return 0;
    u32 *from = first + n + 1, *fill = calloc((size_t)n + 1, sizeof *fill);
    if (!fill) {
        free(first);
        return 0;
    }
    /* counts by target, then their running sum as each target's first, then the sources in place */
    for (int pass = 0; pass < 2; pass++)
        for (u32 i = 0; i < n; i++) {
            const Sm86Insn* in = &c->in[i];
            if (in->pred == SM86_PT && in->pred_not) continue;
            u32 count = 0;
            if (in->op == SM86_BRA) t[count++] = sm86_branch_target(in, i);
            else if (is_brx(in->op))
                count = brx_targets(c, i, t, 64);
            for (u32 k = 0; k < count; k++) {
                if (t[k] >= n) continue;
                if (pass == 0) first[t[k] + 1]++;
                else from[first[t[k]] + fill[t[k]]++] = i;
            }
            if (pass == 0 && i + 1 == n)
                for (u32 k = 0; k < n; k++) first[k + 1] += first[k];
        }
    free(fill);
    c->jump_first = first;
    c->jump_from = from;
    return 1;
}
/* whether the instruction may write register r: the registers from its destination field on, four for those that
   load (a texture's results go to two pairs, the second from bits 64-71), two for the others (a 64-bit result) */
static int may_write(const Sm86Insn* in, u32 r) {
    int op = in->op;
    if (sm86_ops[op].kind == SM86_K_UALU) return 0;
    switch (op) {
        case SM86_BRA:
        case SM86_BRX:
        case SM86_BRXU:
        case SM86_EXIT:
        case SM86_RET:
        case SM86_CALL:
        case SM86_UBRA:
        case SM86_BSSY: case SM86_BSYNC: case SM86_BREAK: case SM86_WARPSYNC: case SM86_KILL: case SM86_BAR:
        case SM86_NOP: case SM86_ERRBAR: case SM86_DEPBAR: case SM86_NANOSLEEP: case SM86_MEMBAR: case SM86_CCTL:
        case SM86_STG: case SM86_STG_NOU: case SM86_STL: case SM86_STL_NOU: case SM86_STS: case SM86_STS_NOU:
        case SM86_RED: case SM86_AST: case SM86_SUST_P: case SM86_SUST_D: case SM86_SUST_P_B: case SM86_SUST_D_B:
        case SM86_ULDC: case SM86_ULDC_IDX: case SM86_ULDCG: case SM86_S2UR: case SM86_R2UR: case SM86_PLOP3:
        case SM86_UPLOP3: case SM86_UVOTE: case SM86_ISETP: case SM86_FSETP: case SM86_DSETP: case SM86_HSETP2:
        case SM86_R2P: case SM86_BMOV_B: case SM86_BMOV_BI: case SM86_BMOV_BL: case SM86_ISBEWR:
            return 0;
        case SM86_TEX: case SM86_TEX_B: case SM86_TLD: case SM86_TLD_B: case SM86_TLD4: case SM86_TLD4_B:
        case SM86_TMML: case SM86_TMML_B: case SM86_TXD: case SM86_TXD_B: case SM86_TXQ: case SM86_TXQ_B: {
            u32 d1 = sm86_field(in, 64, 8);
            if (d1 < SM86_RZ && r >= d1 && r < d1 + 4) return 1;
            return in->dst < SM86_RZ && r >= in->dst && r < in->dst + 4u;
        }
        case SM86_LDG: case SM86_LDG_NOU: case SM86_LDL: case SM86_LDS: case SM86_LDC: case SM86_LDC_BINDLESS:
        case SM86_ALD: case SM86_ATOMG: case SM86_ATOMG_NOU: case SM86_ATOMG_CAS: case SM86_ATOMS: case SM86_ATOMS_NOU:
        case SM86_ATOMS_CAS: case SM86_SULD_P: case SM86_SULD_P_B: case SM86_SULD_D: case SM86_SULD_D_B:
        case SM86_SURED: case SM86_SURED_B: case SM86_SUATOM: case SM86_SUATOM_B: case SM86_SUATOM_CAS_B:
        case SM86_HMMA: case SM86_LDSM:
            return in->dst < SM86_RZ && r >= in->dst && r < in->dst + 4u;
        default: return in->dst < SM86_RZ && r >= in->dst && r < in->dst + 2u;
    }
}
/* the instructions before k on each path, walked back: k - 1 when it goes on to k, and the jumps to k */
#define FLOW_PREDS(k)                                                                                                  \
    do {                                                                                                               \
        if ((k) == 0) ok = 0; /* the program's start: r is not written on this path */                                 \
        if ((k) > 0 && falls_through(&c->in[(k) - 1]) && !seen[(k) - 1]) seen[(k) - 1] = 1, stack[top++] = (k) - 1;    \
        for (u32 j = c->jump_first[k]; j < c->jump_first[(k) + 1]; j++)                                                \
            if (!seen[c->jump_from[j]]) seen[c->jump_from[j]] = 1, stack[top++] = c->jump_from[j];                     \
    } while (0)
/* Whether register r holds one constant at instruction i on every path there: the immediate that an unpredicated
   MOV wrote last on each, with no instruction that may write r after it. 0 when that can't be shown (another
   writer, a guard, a path from the program's start, or a jump the flow can't name) */
static int reg_const_at(Ctx* c, u32 i, u32 r, u32* value) {
    if (r >= SM86_RZ) {
        *value = 0;
        return 1;
    }
    if (!flow_sources(c)) return 0;
    u32 n = c->n, *stack = malloc(((size_t)n + 1) * sizeof *stack), top = 0, found = 0, v = 0;
    u8* seen = calloc((size_t)n + 1, 1);
    int ok = stack && seen;
    if (ok) FLOW_PREDS(i);
    while (ok && top) {
        u32 k = stack[--top];
        const Sm86Insn* in = &c->in[k];
        if (!may_write(in, r)) {
            FLOW_PREDS(k);
            continue;
        }
        if (in->op != SM86_MOV || in->dst != r || in->b.kind != SM86_SRC_IMM || in->pred != SM86_PT || in->pred_not ||
            (found && in->b.value != v)) {
            ok = 0;
            break;
        }
        v = in->b.value;
        found = 1;
    }
    free(stack);
    free(seen);
    if (!ok || !found) return 0;
    *value = v;
    return 1;
}
/* may_write, of the words an instruction really writes where its type tells: an ALU's result is one but for those of
   64 bits, and an LDC's as its size says */
static int writes_word(const Sm86Insn* in, u32 r) {
    if (!may_write(in, r)) return 0;
    if (in->op == SM86_LDC) return r < in->dst + (sm86_field(in, 73, 3) == 5 ? 2u : 1u);
    if (sm86_ops[in->op].kind != SM86_K_ALU) return 1;
    switch (in->op) {
        case SM86_DADD:
        case SM86_DMUL:
        case SM86_DFMA:
        case SM86_F2F64:
        case SM86_F2I64:
        case SM86_I2F64:
        case SM86_FRND64:
        case SM86_IMAD_WIDE: return 1;
        default: return r == in->dst;
    }
}
/* The one instruction whose write of register r reaches instruction i on every path there, without a guard, the word
   its destination field names. -1 when that can't be shown */
static int reaching_write(Ctx* c, u32 i, u32 r) {
    if (r >= SM86_RZ || !flow_sources(c)) return -1;
    u32 n = c->n, *stack = malloc(((size_t)n + 1) * sizeof *stack), top = 0;
    u8* seen = calloc((size_t)n + 1, 1);
    int ok = stack && seen, found = -1;
    if (ok) FLOW_PREDS(i);
    while (ok && top) {
        u32 k = stack[--top];
        const Sm86Insn* in = &c->in[k];
        if (!writes_word(in, r)) {
            FLOW_PREDS(k);
            continue;
        }
        if (in->dst != r || in->pred != SM86_PT || in->pred_not || (found >= 0 && (u32)found != k)) ok = 0;
        found = (int)k;
    }
    free(stack);
    free(seen);
    return ok ? found : -1;
}
#undef FLOW_PREDS
/* a packed texel offset's component k, bits signed bits from k * bits: a constant, or read from x */
static u32 offset_part(Ctx* c, int known, u32 value, u32 x, u32 k, u32 bits) {
    if (known) {
        u32 raw = value >> (k * bits) & ((1u << bits) - 1);
        return spv_emit_constant_u32(&c->e, c->t_i32, raw >> (bits - 1) ? raw | ~((1u << bits) - 1) : raw);
    }
    return spv_emit_bitfield_sextract(&c->e, c->t_i32, spv_emit_bitcast(&c->e, c->t_i32, x), cu(c, k * bits),
                                      cu(c, bits));
}
/* the offset a texture instruction packs in a register, as the integer vector of dims components SPIR-V takes (a
   1D texture's y, bound as 2D, is 0): constant when known */
static u32 offset_vector(Ctx* c, int known, u32 value, u32 x, u32 dims, u32 used, u32 bits) {
    u32 v[3];
    for (u32 k = 0; k < dims; k++)
        v[k] = k < used ? offset_part(c, known, value, x, k, bits) : spv_emit_constant_u32(&c->e, c->t_i32, 0);
    u32 type = spv_emit_type_vector(&c->e, c->t_i32, dims);
    if (known) return dims == 2 ? spv_emit_constant_vec2(&c->e, type, v[0], v[1]) : spv_emit_constant_vec3(&c->e, type, v[0], v[1], v[2]);
    return dims == 2 ? spv_emit_vec2_construct(&c->e, type, v[0], v[1]) : spv_emit_vec3_construct(&c->e, type, v[0], v[1], v[2]);
}

/* the texture slot of a handle at byte offset off of a bank */
/* the texture slot of the handle at c[bank][off] with that view: ms for a multisampled 2D texture (ShTex.flags bit 1,
   which the renderer binds a multisampled view for) */
static int tex_slot(Ctx* c, u32 bank, u32 off, u32 type, u32 array, u32 shadow, u32 ms) {
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ntex; i++) {
        const ShTex* t = &si->tex[i];
        if (t->bl_cbuf == bank && t->cbuf_off == off / 4 && t->type == type && t->array == array &&
            t->shadow == shadow && !!(t->flags & 2) == !!ms && !(t->flags & SNX_TEX_INDEXED))
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
    t.flags = ms ? 2 : 0;
    si->tex[i] = t;
    /* a 1D texture is bound as a 2D one (shader.h) */
    c->tex[i] = spv_emit_sampler_image(&c->e, c->t_f32, (u32)shader_vk_set(c->stage), SNX_VKB_TEX + (u32)i,
                                       type == 0 ? 1 : type, (int)array, (int)shadow, ms ? 1 : 0, &c->tex_sampled[i]);
    add_interface(c, c->tex[i]);
    return i;
}
/* The slot of a table of handles (SNX_TEX_INDEXED): count of them from byte off of a bank, stride bytes apart. Slots
   of a kind share its arrays, and the map from a table's element to the arrays' is the renderer's (tex_tables_set),
   as spirv_translate.c declares them for Maxwell's */
static int tex_slot_indexed(Ctx* c, u32 bank, u32 off, u32 type, u32 array, u32 shadow, u32 stride, u32 count) {
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ntex; i++) {
        const ShTex* t = &si->tex[i];
        if ((t->flags & SNX_TEX_INDEXED) && t->bl_cbuf == bank && t->cbuf_off == off / 4 && t->type == type &&
            t->array == array && t->shadow == shadow && t->stride == stride && t->count == count)
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
    t.flags = SNX_TEX_INDEXED;
    t.stride = (u16)stride;
    t.count = (u16)count;
    si->tex[i] = t;
    u32 set = (u32)shader_vk_set(c->stage), dim = type == 0 ? 1 : type;
    int same = 0;
    while (same < i && !((si->tex[same].flags & SNX_TEX_INDEXED) &&
                         shader_same_indexed_kind(si->tex, si->tex_int_mask, si->tex_sint_mask, same, i)))
        same++;
    if (same < i) {
        c->tex_array[i] = c->tex_array[same];
        c->tex_sampled[i] = c->tex_sampled[same];
        c->tex_depth[i] = c->tex_depth[same];
        c->tex_depth_type[i] = c->tex_depth_type[same];
    } else {
        c->tex_array[i] = spv_emit_sampler_image_array(&c->e, c->t_f32, c->t_u32, set, SNX_VKB_TEXARRAY, dim,
                                                       (int)array, 0, SNX_TEX_ARRAY, &c->tex_sampled[i]);
        add_interface(c, c->tex_array[i]);
        if (shadow) {
            c->tex_depth[i] = spv_emit_sampler_image_array(&c->e, c->t_f32, c->t_u32, set, SNX_VKB_TEXDEPTH, dim,
                                                           (int)array, 1, SNX_TEX_ARRAY, &c->tex_depth_type[i]);
            add_interface(c, c->tex_depth[i]);
        }
    }
    if (shadow && !c->tex_func) {
        c->tex_func = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, set, SNX_VKB_TEXFUNCS,
                                                  SNX_TEX_ARRAY / 4, &c->tex_func_ptr);
        add_interface(c, c->tex_func);
    }
    if (!c->tex_map) {
        c->tex_map = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, set, SNX_VKB_TEXMAP,
                                                 SH_INDEXED_TABLES * SNX_TEX_ARRAY / 4, &c->tex_map_ptr);
        add_interface(c, c->tex_map);
    }
    return i;
}
/* a slot's table: the first slot of each table in their order, as the renderer numbers them (stage_set_) */
static u32 indexed_table(const ShaderInfo* si, int slot) {
    u32 table = 0;
    for (int i = 0; i <= slot; i++) {
        int seen = !(si->tex[i].flags & SNX_TEX_INDEXED);
        for (int j = 0; j < i && !seen; j++)
            seen = (si->tex[j].flags & SNX_TEX_INDEXED) && shader_same_indexed_table(&si->tex[j], &si->tex[i]);
        if (seen) continue;
        if (shader_same_indexed_table(&si->tex[i], &si->tex[slot])) return table;
        table++;
    }
    return table;
}
/* the word at an index of a uniform array of vec4s */
static u32 uniform_word(Ctx* c, u32 pointer, u32 buffer, u32 index) {
    return spv_emit_uniform_component(&c->e, pointer, buffer, c->zero,
                                      spv_emit_shift_right_u32(&c->e, c->t_u32, index, cu(c, 2)),
                                      spv_emit_bitwise_and_u32(&c->e, c->t_u32, index, cu(c, 3)), c->t_u32);
}
/* For this instruction, an indexed slot's tex[] is the element its LDC chose, clamped to the array, as
   shader_direct_swap_element does. Returns the array's element */
static u32 indexed_element(Ctx* c, int slot, u32 var) {
    u32 limit = cu(c, SNX_TEX_ARRAY - 1), index = spv_emit_load(&c->e, c->t_u32, var);
    index = spv_emit_select(&c->e, c->t_u32, spv_emit_u_less(&c->e, c->t_bool, limit, index), limit, index);
    index = spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, indexed_table(c->si, slot) * SNX_TEX_ARRAY));
    u32 element = uniform_word(c, c->tex_map_ptr, c->tex_map, index);
    c->tex[slot] = spv_emit_sampler_array_element(&c->e, c->tex_sampled[slot], c->tex_array[slot], element);
    return element;
}
/* A depth compare of an indexed texture, whose element may be a depth texture or a color one: both are sampled, the
   color's first component compared here with the element's function, and SNX_VKB_TEXFUNCS says which holds. As
   shader_dc_compare_indexed does */
static u32 sample_indexed_shadow(Ctx* c, int slot, u32 element, u32 coord, u32 dref, u32 mode, u32 level, u32 offset) {
    static const u32 condition[8] = {0, 1, 2, 3, 4, 5, 6, 15}; /* spv_emit_fcompare's codes for NEVER..ALWAYS */
    u32 color = spv_emit_vec4_extract(
        &c->e, c->t_f32,
        spv_emit_sample_image(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, 0, mode, level, offset, 0),
        0);
    u32 meta = uniform_word(c, c->tex_func_ptr, c->tex_func, element);
    u32 func = spv_emit_bitwise_and_u32(&c->e, c->t_u32, meta, cu(c, 7)), one = cf(c, 1.0f), none = cf(c, 0.0f);
    u32 result = none;
    for (u32 i = 0; i < 8; i++) {
        u32 pass =
            spv_emit_select(&c->e, c->t_f32, spv_emit_fcompare(&c->e, c->t_bool, condition[i], dref, color), one, none);
        result = spv_emit_select(&c->e, c->t_f32, spv_emit_i_equal(&c->e, c->t_bool, func, cu(c, i)), pass, result);
    }
    u32 view = spv_emit_sampler_array_element(&c->e, c->tex_depth_type[slot], c->tex_depth[slot], element);
    u32 depth =
        spv_emit_sample_image(&c->e, c->t_f32, c->tex_depth_type[slot], view, coord, dref, mode, level, offset, 0);
    u32 is_depth =
        spv_emit_i_equal(&c->e, c->t_bool, spv_emit_bitwise_and_u32(&c->e, c->t_u32, meta, cu(c, 8)), cu(c, 8));
    return spv_emit_select(&c->e, c->t_f32, is_depth, depth, result);
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
static int tex_operands(const Sm86Insn* in, u32 n0, u32 n1, u32* regs) {
    u32 a = sm86_field(in, 24, 8), b = sm86_field(in, 32, 8), n = n0 + n1;
    if (sm86_field(in, 60, 1)) {
        if (n > 4) return 0;
        u32 first = n <= 2 ? 1 : 2;
        for (u32 k = 0; k < n; k++)
            regs[k] = k < first ? (a >= SM86_RZ ? a : a + k) : (b >= SM86_RZ ? b : b + k - first);
        return 1;
    }
    for (u32 k = 0; k < n0; k++) regs[k] = a >= SM86_RZ ? a : a + k;
    for (u32 k = 0; k < n1; k++) regs[n0 + k] = b >= SM86_RZ ? b : b + k;
    return 1;
}
/* the components of a vec4 result that the channel mask selects, into dst and dst + 1, then dst1 and dst1 + 1 (or on
   from dst when dst1 is RZ). With .F16 (bit 79, NAK's note on TLD) a float result is two halves a register: the
   first two components into dst (the first in the low half), the next two into dst1 (or dst + 1), a lone last one
   with a zero high half. One game's text programs take an alpha that way (HADD2.F32 R, -RZ, R.H0_H0), and with each
   component in a register of its own their glyphs read the low bits of a float */
static void tex_results(Ctx* c, const Sm86Insn* in, u32 value, u32 scalar) {
    u32 mask = sm86_field(in, 72, 4), d0 = in->dst, d1 = sm86_field(in, 64, 8), written = 0;
    if (scalar == c->t_f32 && sm86_field(in, 79, 1)) {
        u32 half[4], n = 0;
        for (u32 k = 0; k < 4; k++)
            if (mask & (1u << k)) half[n++] = spv_emit_vec4_extract(&c->e, scalar, value, k);
        for (u32 p = 0; 2 * p < n; p++) {
            u32 pair =
                spv_emit_vec2_construct(&c->e, c->t_v2f, half[2 * p], 2 * p + 1 < n ? half[2 * p + 1] : cf(c, 0.0f));
            u32 r = p == 0 || d1 >= SM86_RZ ? (d0 >= SM86_RZ ? d0 : d0 + p) : d1;
            wr(c, r, SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, pair));
        }
        return;
    }
    for (u32 k = 0; k < 4; k++) {
        if (!(mask & (1u << k))) continue;
        u32 v = spv_emit_vec4_extract(&c->e, scalar, value, k);
        if (scalar != c->t_u32) v = to_u(c, v);
        u32 r = written < 2 || d1 >= SM86_RZ ? (d0 >= SM86_RZ ? d0 : d0 + written) : d1 + written - 2;
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
static void texture(Ctx* c, const Sm86Insn* in) {
    int bindless = in->op == SM86_TEX_B, op = bindless ? SM86_TEX : in->op;
    u32 type, array, coords;
    if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) {
        fail(c, "%s dimension 6", sm86_ops[op].name);
        return;
    }
    u32 shadow = op == SM86_TMML ? 0 : sm86_field(in, 78, 1);
    u32 lod_mode = op == SM86_TEX ? sm86_field(in, 87, 3) : 0;
    /* the texel offset (NAK's TexOffsetMode): 1 one offset packed in a register (four bits a component, TLD4's eight),
       2 TLD4's own offset for each of its four texels, eight bits a component in two registers */
    u32 offset = op == SM86_TEX ? sm86_field(in, 76, 1) : op == SM86_TLD4 ? sm86_field(in, 76, 2) : 0;
    if (offset == 3 || (offset && type == 3)) {
        fail(c, "%s with texel offset mode %u%s", sm86_ops[op].name, offset, type == 3 ? " on a cube" : "");
        return;
    }
    if (lod_mode > 3) {
        fail(c, "TEX LOD mode %u", lod_mode);
        return;
    }
    if (op == SM86_TMML && c->stage != ST_FS) {
        fail(c, "TMML outside a fragment program");
        return;
    }
    u32 handle = sm86_field(in, 40, 19);
    int slot = bindless ? (int)c->handle[c->cur].slot - 1
                        : tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, shadow, 0);
    if (slot < 0) return;
    /* list1: a TEX.B's handle (prepare_handles), the LOD or bias, the offset, the depth reference
       (nak_nir_lower_tex.c) */
    u32 n0 = array + coords, n1 = (u32)bindless + (lod_mode == 2 || lod_mode == 3) + offset + shadow, regs[8];
    if (!tex_operands(in, n0, n1, regs)) {
        fail(c, "%s with %u operands in the scalar form", sm86_ops[op].name, n0 + n1);
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
    u32 next = n0 + (u32)bindless, level = 0, dref = 0, off_reg = SM86_RZ;
    int indexed = (c->si->tex[slot].flags & SNX_TEX_INDEXED) != 0;
    u32 element = indexed ? indexed_element(c, slot, c->handle[c->handle[c->cur].ldc - 1].var) : 0;
    if (lod_mode == 2 || lod_mode == 3) level = to_f(c, rd(c, regs[next++]));
    if (offset) {
        off_reg = regs[next];
        next += offset;
    }
    if (shadow) dref = to_f(c, rd(c, regs[next++]));
    /* SPIR-V's offset has a component for each coordinate but the layer */
    u32 dims = type == 2 ? 3 : 2, used = type == 0 ? 1 : dims;
    u32 result;
    if (op == SM86_TLD4 && offset) {
        /* Offset, which a gather may take from a register (ImageGatherExtended). Each texel's own offset: four
           gathers, each texel from the one with its offset, as SPIR-V's ConstOffsets defines it */
        spv_emit_image_gather_extended_capability(&c->e);
        u32 comp = cu(c, sm86_field(in, 87, 2)), v[4];
        if (offset == 1) {
            u32 o = offset_vector(c, 0, 0, rd(c, off_reg), dims, used, 8);
            result = spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, comp, dref, o);
        } else {
            for (u32 k = 0; k < 4; k++) {
                u32 word = rd(c, off_reg >= SM86_RZ ? off_reg : off_reg + k / 2);
                u32 type2 = spv_emit_type_vector(&c->e, c->t_i32, 2);
                u32 o = spv_emit_vec2_construct(&c->e, type2, offset_part(c, 0, 0, word, (k % 2) * 2, 8),
                                                offset_part(c, 0, 0, word, (k % 2) * 2 + 1, 8));
                v[k] = spv_emit_vec4_extract(&c->e, c->t_f32,
                                             spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot],
                                                                      c->tex[slot], coord, comp, dref, o),
                                             k);
            }
            result = spv_emit_vec4_construct(&c->e, c->t_v4f, v);
        }
    } else if (op == SM86_TLD4) {
        result = spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord,
                                          cu(c, sm86_field(in, 87, 2)), dref, 0);
    } else if (op == SM86_TMML) {
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
        u32 rtype = shadow ? c->t_f32 : c->t_v4f, const_offset = 0;
        if (offset) {
            /* Vulkan takes a sample's offset as a constant only (ConstOffset): the register's when every path
               sets it to one, else the coordinates move by the offset over the size of the level sampled */
            u32 value = 0;
            if (reg_const_at(c, c->cur, off_reg, &value))
                const_offset = offset_vector(c, 1, value, 0, dims, used, 4);
            else {
                u32 o = offset_vector(c, 0, 0, rd(c, off_reg), dims, used, 4), at;
                if (mode == 1)
                    at = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_MAX,
                                              spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_FLOOR, level),
                                              cf(c, 0.0f));
                else { /* the level the implicit LOD, with its bias, accesses */
                    spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
                    at = spv_emit_glsl_unary(
                        &c->e, c->t_f32, c->glsl, SPV_GLSL_FLOOR,
                        spv_emit_vec4_extract(&c->e, c->t_f32,
                                              spv_emit_image_query_lod(&c->e, c->t_v2f, c->tex_sampled[slot],
                                                                       c->tex[slot], coord),
                                              0));
                }
                spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
                u32 size = spv_emit_image_query_size_lod(&c->e, spv_emit_type_vector(&c->e, c->t_i32, dims + array),
                                                         c->tex_sampled[slot], c->tex[slot],
                                                         spv_emit_convert_f_to_s(&c->e, c->t_i32, at));
                u32 moved[4];
                for (u32 k = 0; k < count; k++) {
                    moved[k] = spv_emit_vec4_extract(&c->e, c->t_f32, coord, k);
                    if (k < used)
                        moved[k] = spv_emit_fadd(
                            &c->e, c->t_f32, moved[k],
                            spv_emit_fdiv(&c->e, c->t_f32,
                                          spv_emit_convert_s_to_f(&c->e, c->t_f32,
                                                                  spv_emit_vec4_extract(&c->e, c->t_i32, o, k)),
                                          spv_emit_convert_s_to_f(&c->e, c->t_f32,
                                                                  spv_emit_vec4_extract(&c->e, c->t_i32, size, k))));
                }
                coord = coord_vector(c, moved, count);
            }
        }
        result = indexed && shadow ? sample_indexed_shadow(c, slot, element, coord, dref, mode, level, const_offset)
                                   : spv_emit_sample_image(&c->e, rtype, c->tex_sampled[slot], c->tex[slot], coord,
                                                           dref, mode, level, const_offset, 0);
        if (shadow) {
            u32 v[4] = {result, cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
            result = spv_emit_vec4_construct(&c->e, c->t_v4f, v);
        }
    }
    tex_results(c, in, result, c->t_f32);
}
/* TXD: a sample with explicit gradients. Unlike TEX's, its registers are in the order of Maxwell's TXD: from a the
   coordinates and then the layer, from b each coordinate's change along x and along y in turn */
static void texture_grad(Ctx* c, const Sm86Insn* in) {
    u32 type, array, coords;
    if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) {
        fail(c, "TXD dimension 6");
        return;
    }
    if (sm86_field(in, 76, 1)) {
        fail(c, "TXD with a texel offset");
        return;
    }
    u32 handle = sm86_field(in, 40, 19);
    int slot = tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, 0, 0);
    if (slot < 0) return;
    u32 a = sm86_field(in, 24, 8), b = sm86_field(in, 32, 8), f[4], dx[3], dy[3], count = 0;
#define TXD_REG(base, k) ((base) < SM86_RZ ? (base) + (k) : SM86_RZ)
    for (u32 k = 0; k < coords; k++, count++) {
        f[count] = to_f(c, rd(c, TXD_REG(a, k)));
        dx[count] = to_f(c, rd(c, TXD_REG(b, 2 * k)));
        dy[count] = to_f(c, rd(c, TXD_REG(b, 2 * k + 1)));
    }
    /* a 1D texture is bound as a 2D one, with no change along its height */
    if (type == 0) {
        f[count] = dx[count] = dy[count] = cf(c, 0.0f);
        count++;
    }
    u32 gx = coord_vector(c, dx, count), gy = coord_vector(c, dy, count);
    if (array)
        f[count++] = spv_emit_convert_u_to_f(
            &c->e, c->t_f32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, rd(c, TXD_REG(a, coords)), cu(c, 0xffff)));
#undef TXD_REG
    tex_results(
        c, in,
        spv_emit_sample_grad(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord_vector(c, f, count), gx, gy, 0),
        c->t_f32);
}
/* TLD: a texel at integer coordinates and an explicit level (bits 87-89, NAK's TexLodMode: 1 the base level, 3 a
   level register). Its operands: the layer and the coordinates, then the level (nak_nir_lower_tex.c) */
static void texel_fetch(Ctx* c, const Sm86Insn* in) {
    u32 type, array, coords, lod_mode = sm86_field(in, 87, 3);
    if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords) || type == 3) {
        fail(c, "TLD dimension %u", sm86_field(in, 61, 3));
        return;
    }
    /* bit 78: a multisampled 2D texture, which has one level (LOD mode 1, zero), and the sample in list1 */
    u32 offset = sm86_field(in, 76, 1), ms = sm86_field(in, 78, 1);
    if ((lod_mode != 1 && lod_mode != 3) || (ms && (type != 1 || lod_mode != 1))) {
        fail(c, "TLD of %s, LOD mode %u", ms ? "a multisampled texture" : "a texture", lod_mode);
        return;
    }
    u32 handle = sm86_field(in, 40, 19);
    int slot = tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, 0, ms);
    if (slot < 0) return;
    /* list1: the level, the offset (four bits a component), then the sample, as Maxwell's TLD orders them */
    u32 n0 = array + coords, n1 = (lod_mode == 3) + offset + ms, regs[8];
    if (!tex_operands(in, n0, n1, regs)) {
        fail(c, "TLD with %u operands in the scalar form", n0 + n1);
        return;
    }
    /* the coordinates, a 1D texture's as a 2D one's, then the layer */
    u32 v[4], count = 0;
    for (u32 k = 0; k < coords; k++) v[count++] = rd(c, regs[array + k]);
    if (type == 0) v[count++] = c->zero;
    /* a fetch's offset adds to its coordinates: the texel is the one there, as Offset's would be */
    if (offset)
        for (u32 k = 0; k < coords; k++)
            v[k] = spv_emit_iadd_u32(&c->e, c->t_u32, v[k],
                                     spv_emit_bitcast(&c->e, c->t_u32,
                                                      offset_part(c, 0, 0, rd(c, regs[n0 + (lod_mode == 3)]), k, 4)));
    if (array) v[count++] = spv_emit_bitwise_and_u32(&c->e, c->t_u32, rd(c, regs[0]), cu(c, 0xffff));
    u32 ivec = spv_emit_type_vector(&c->e, c->t_i32, count);
    u32 uvec = spv_emit_type_vector(&c->e, c->t_u32, count);
    u32 coord = spv_emit_bitcast(&c->e, ivec,
                                 count == 2 ? spv_emit_vec2_construct(&c->e, uvec, v[0], v[1])
                                            : spv_emit_vec3_construct(&c->e, uvec, v[0], v[1], v[2]));
    u32 level =
        lod_mode == 3 ? spv_emit_bitcast(&c->e, c->t_i32, rd(c, regs[n0])) : spv_emit_bitcast(&c->e, c->t_i32, c->zero);
    u32 texel = ms ? spv_emit_image_fetch_sample(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord,
                                                 spv_emit_bitcast(&c->e, c->t_i32, rd(c, regs[n0 + offset])))
                   : spv_emit_image_fetch(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, level);
    tex_results(c, in, texel, c->t_f32);
}
/* TXQ of the dimensions (bits 62-63 = 0): width, height, depth or layers, and the levels, at the level in register
   a. TXQ names no dimension: the slot of a texture instruction with the same handle, else a 2D texture */
static void texture_query(Ctx* c, const Sm86Insn* in) {
    if (sm86_field(in, 62, 2)) {
        fail(c, "TXQ query %u", sm86_field(in, 62, 2));
        return;
    }
    u32 handle = sm86_field(in, 40, 19), bank = (handle >> 14) & 31, off = handle & 0x3fff;
    int slot = -1;
    for (int i = 0; i < c->si->ntex && slot < 0; i++)
        if (c->si->tex[i].bl_cbuf == bank && c->si->tex[i].cbuf_off == off && !(c->si->tex[i].flags & SNX_TEX_INDEXED))
            slot = i;
    /* a TLD of a multisampled 2D texture with the handle, anywhere in the program, names its view */
    u32 ms = 0;
    for (u32 k = 0; k < c->n && slot < 0 && !ms; k++)
        ms = c->in[k].op == SM86_TLD && sm86_field(&c->in[k], 78, 1) && sm86_field(&c->in[k], 40, 19) == handle;
    if (slot < 0) slot = tex_slot(c, bank, off * 4, 1, 0, 0, ms);
    if (slot < 0) return;
    const ShTex* t = &c->si->tex[slot];
    ms = t->flags & 2;
    u32 dims = (t->type == 2 ? 3 : 2) + t->array; /* a 1D texture is a 2D one, a cube's size is its face's */
    if (t->type == 3) dims = 2 + t->array;
    spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
    /* a multisampled texture has one level, and its size takes none */
    u32 size = spv_emit_image_query_size_lod(&c->e, spv_emit_type_vector(&c->e, c->t_u32, dims), c->tex_sampled[slot],
                                             c->tex[slot],
                                             ms ? 0 : spv_emit_bitcast(&c->e, c->t_i32, rd(c, sm86_field(in, 24, 8))));
    u32 v[4];
    for (u32 k = 0; k < 3; k++) v[k] = k < dims ? SPV(&c->e, SPV_OP_COMPOSITE_EXTRACT, c->t_u32, size, k) : cu(c, 1);
    if (t->type == 0) v[1] = cu(c, 1);
    v[3] = ms ? cu(c, 1) : spv_emit_image_query_levels(&c->e, c->t_u32, c->tex_sampled[slot], c->tex[slot]);
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
static void surface(Ctx* c, const Sm86Insn* in) {
    int store = in->op == SM86_SUST_P, atomic = in->op == SM86_SURED || in->op == SM86_SUATOM;
    u32 sdim, array, coords, type;
    if (!image_dim(sm86_field(in, 61, 3), &sdim, &array, &coords, &type)) {
        fail(c, "%s dimension %u", sm86_ops[in->op].name, sm86_field(in, 61, 3));
        return;
    }
    u32 handle = sm86_field(in, 40, 19), bank = (handle >> 14) & 31, off = handle & 0x3fff;
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
    u32 a = sm86_field(in, 24, 8), v[4], count = 0;
    for (u32 k = 0; k < coords; k++) {
        if (sdim == 0 && k == 1) v[count++] = c->zero;
        v[count++] = rd(c, a >= SM86_RZ ? a : a + k);
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
        /* SUATOM: the same, its operation 4 bits wide (8 EXCH), and the prior value to the destination */
        int returns = in->op == SM86_SUATOM;
        u32 how = sm86_field(in, 87, returns ? 4 : 3), atype = sm86_field(in, 73, 3);
        u32 opcode = how == 8 && returns ? SPV_OP_ATOMIC_EXCHANGE : how < 8 && atype <= 1 ? ops[atype][how] : 0;
        if (atype > 1 || !opcode) {
            fail(c, "%s of type %u, operation %u", sm86_ops[in->op].name, atype, how);
            return;
        }
        u32 ptr = spv_emit_image_texel_pointer(&c->e, c->t_u32, c->img[slot], coord, c->zero);
        u32 prior = spv_emit_atomic_u32(&c->e, c->t_u32, opcode, ptr, rd(c, sm86_field(in, 32, 8)));
        if (returns && in->dst < SM86_RZ) wr(c, in->dst, prior);
        return;
    }
    /* the texel in the image's own type: the register words as floats (kind 0) or integers (2, 3); a component
       that the mask leaves out is 0, the alpha 1 */
    u32 kind = si->img[slot].kind, scalar = image_scalar(c, kind);
    u32 vec4 = spv_emit_type_vec4(&c->e, scalar);
    u32 mask = sm86_field(in, 72, 4);
    if (store) {
        u32 data = sm86_field(in, 32, 8), f[4], next = 0;
        for (u32 k = 0; k < 4; k++) {
            if (mask & (1u << k))
                f[k] = spv_emit_bitcast(&c->e, scalar, rd(c, data >= SM86_RZ ? data : data + next++));
            else
                f[k] = kind == 0 ? cf(c, k == 3 ? 1.0f : 0.0f) : spv_emit_bitcast(&c->e, scalar, cu(c, k == 3));
        }
        spv_emit_image_write(&c->e, c->img_type[slot], c->img[slot], coord, spv_emit_vec4_construct(&c->e, vec4, f));
    } else {
        u32 texel = spv_emit_image_read(&c->e, vec4, c->img_type[slot], c->img[slot], coord);
        u32 d = in->dst, written = 0;
        for (u32 k = 0; k < 4; k++)
            if (mask & (1u << k)) {
                wr(c, d >= SM86_RZ ? d : d + written, to_u(c, spv_emit_vec4_extract(&c->e, scalar, texel, k)));
                written++;
            }
    }
}

static u32 system_value(Ctx* c, u32 sr);

/* ---- the warp's matrix instructions (HMMA, LDSM, MOVM), as the lanes of an invocation's own 32 of a subgroup of 32 or
   64 (NVIDIA's warp: lane l of it is the local invocation index's l % 32), each one's registers spread over the lanes
   as PTX's mma.sync, ldmatrix and movmatrix lay them out. A lane reads another's register with a shuffle */
static u32 warp_lane(Ctx* c) { return spv_emit_bitwise_and_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, 31)); }
/* value as lane j (0-31) of the invocation's own 32 holds it */
static u32 warp_read(Ctx* c, u32 value, u32 j) {
    u32 own = spv_emit_bitwise_and_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, ~31u));
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
    return SPV(&c->e, 345 /* OpGroupNonUniformShuffle */, c->t_u32, cu(c, 3), value,
               spv_emit_bitwise_or_u32(&c->e, c->t_u32, own, j));
}
static u32 half_pair(Ctx* c, u32 bits) { return SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, bits); }
/* HMMA (0x23c): D = A B + C on the warp's fragments, as PTX's mma.sync.aligned.m16n8k16 (bit 75) or m16n8k8 .row.col
   with f16 A and B, and f16 (bit 76 clear) or f32 C and D (the encodings of CuAssembler's sm_86 records: bits 72-90 8
   16816.F16, 0x18 16816.F32, 0 1688.F16, 0x10 1688.F32; BF16, TF32 and 1684 are other bits). D at 16, A at 24, B at
   32, C at 64 (RZ: zero), each the first of its registers. With g = l / 4 and t = l % 4, lane l holds: A's rows g
   (a0) and g + 8 (a1) at columns 2t and 2t + 1, and for k16 the same at columns 2t + 8 and 2t + 9 (a2, a3); B's column
   g at rows 2t and 2t + 1 (b0), for k16 also 2t + 8 and 2t + 9 (b1); C and D's rows g and g + 8 at columns 2t and
   2t + 1, a register for each element in f32 (c0 c1 row g, c2 c3 row g + 8) or a pair for each row in f16. Each lane
   gathers the A pairs of its two rows from lanes 4g to 4g + 3 and the B pairs of its two columns from lanes 8t + p and
   8t + 4 + p (32 shuffles for k16), and sums the products in f32, which an f16 D then rounds to */
static void hmma(Ctx* c, const Sm86Insn* in) {
    u32 v = sm86_field(in, 72, 19);
    if (v & ~0x18u) {
        fail(c, "HMMA variant %#x", v);
        return;
    }
    int k16 = (int)(v >> 3 & 1), f32 = (int)(v >> 4 & 1);
    u32 ra = sm86_field(in, 24, 8), rb = sm86_field(in, 32, 8), rc = sm86_field(in, 64, 8);
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    for (k = 0; k < na; k++) a[k] = ra >= SM86_RZ ? c->zero : rd(c, ra + k);
    for (k = 0; k < nb; k++) b[k] = rb >= SM86_RZ ? c->zero : rd(c, rb + k);
    for (k = 0; k < ncd; k++) cc[k] = rc >= SM86_RZ ? c->zero : rd(c, rc + k);
    u32 lane = warp_lane(c);
    u32 g4 = spv_emit_bitwise_and_u32(&c->e, c->t_u32, lane, cu(c, 28));
    u32 t8 =
        spv_emit_shift_left_u32(&c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, lane, cu(c, 3)), cu(c, 3));
    u32 acc[2][2]; /* [row g, g + 8][column 2t, 2t + 1] */
    for (u32 i = 0; i < 2; i++) {
        if (f32) {
            acc[i][0] = to_f(c, cc[2 * i]);
            acc[i][1] = to_f(c, cc[2 * i + 1]);
        } else {
            u32 pair = half_pair(c, cc[i]);
            acc[i][0] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 0);
            acc[i][1] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 1);
        }
    }
    for (u32 p = 0; p < 4; p++) {
        u32 la = spv_emit_bitwise_or_u32(&c->e, c->t_u32, g4, cu(c, p));
        u32 lb[2] = {spv_emit_bitwise_or_u32(&c->e, c->t_u32, t8, cu(c, p)),
                     spv_emit_bitwise_or_u32(&c->e, c->t_u32, t8, cu(c, 4 + p))};
        u32 av[4], bv[2][2];
        for (k = 0; k < na; k++) av[k] = half_pair(c, warp_read(c, a[k], la));
        for (u32 j = 0; j < 2; j++)
            for (k = 0; k < nb; k++) bv[j][k] = half_pair(c, warp_read(c, b[k], lb[j]));
        for (u32 h = 0; h < nb; h++) /* the k halves: A's registers 2h (row g) and 2h + 1 (row g + 8), B's h */
            for (u32 i = 0; i < 2; i++)
                for (u32 j = 0; j < 2; j++)
                    acc[i][j] = op2(c, SPV_OP_F_ADD, c->t_f32, acc[i][j],
                                    op2(c, 148 /* OpDot */, c->t_f32, av[2 * h + i], bv[j][h]));
    }
    u32 d = in->dst;
    if (d >= SM86_RZ) return;
    if (f32) {
        wr(c, d, to_u(c, acc[0][0]));
        wr(c, d + 1, to_u(c, acc[0][1]));
        wr(c, d + 2, to_u(c, acc[1][0]));
        wr(c, d + 3, to_u(c, acc[1][1]));
    } else
        for (u32 i = 0; i < 2; i++)
            wr(c, d + i,
               SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                       spv_emit_vec2_construct(&c->e, c->t_v2f, acc[i][0], acc[i][1])));
}
/* a 16-bit element of shared memory at the byte address, in the low half */
static u32 shared_u16(Ctx* c, u32 address) {
    u32 word = spv_emit_load(&c->e, c->t_u32, mem_ptr(c, 0, address, 0));
    u32 shift = spv_emit_shift_left_u32(&c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, address, cu(c, 2)),
                                        cu(c, 3));
    return spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, word, shift),
                                    cu(c, 0xffff));
}
/* LDSM.16 (0x83b): 1, 2 or 4 (bits 72-73) 8x8 matrices of 16-bit elements from shared memory into the destination's
   registers, one for each, as PTX's ldmatrix.sync.aligned.m8n8: lanes 8m to 8m + 7 give the addresses of matrix m's
   rows 0 to 7 (the register at 24 plus the signed offset at 40-63; bit 91, a uniform register at 32, is not taken),
   and lane l receives row l / 4's elements at columns 2(l % 4) and 2(l % 4) + 1, or transposed (MT88, bit 78) column
   l / 4's at rows 2(l % 4) and 2(l % 4) + 1 */
static void ldsm(Ctx* c, const Sm86Insn* in) {
    u32 count = sm86_field(in, 72, 2), trans = sm86_field(in, 78, 1);
    if (count > 2 || sm86_field(in, 74, 4) || sm86_field(in, 79, 12) || sm86_field(in, 91, 1)) {
        fail(c, "LDSM variant %#x", sm86_field(in, 72, 20));
        return;
    }
    shared_var(c); /* before mem_ptr, which bounds the index by its size: as LDS does, for a program whose LDSM comes
                      before any other access to shared memory */
    u32 imm = sm86_field(in, 40, 24), bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    u32 addr = spv_emit_iadd_u32(&c->e, c->t_u32, rd(c, sm86_field(in, 24, 8)), cu(c, bias));
    u32 lane = warp_lane(c), g = spv_emit_shift_right_u32(&c->e, c->t_u32, lane, cu(c, 2));
    u32 t = spv_emit_bitwise_and_u32(&c->e, c->t_u32, lane, cu(c, 3)), out[4];
    for (u32 m = 0; m < (1u << count); m++) {
        if (!trans) {
            u32 row = warp_read(c, addr, spv_emit_iadd_u32(&c->e, c->t_u32, g, cu(c, 8 * m)));
            u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, row, spv_emit_shift_left_u32(&c->e, c->t_u32, t, cu(c, 2)));
            out[m] = spv_emit_load(&c->e, c->t_u32, mem_ptr(c, 0, at, 0));
        } else {
            u32 r0 =
                spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_shift_left_u32(&c->e, c->t_u32, t, cu(c, 1)), cu(c, 8 * m));
            u32 col = spv_emit_shift_left_u32(&c->e, c->t_u32, g, cu(c, 1));
            u32 lo = shared_u16(c, spv_emit_iadd_u32(&c->e, c->t_u32, warp_read(c, addr, r0), col));
            u32 hi = shared_u16(
                c, spv_emit_iadd_u32(&c->e, c->t_u32,
                                     warp_read(c, addr, spv_emit_iadd_u32(&c->e, c->t_u32, r0, cu(c, 1))), col));
            out[m] =
                spv_emit_bitwise_or_u32(&c->e, c->t_u32, lo, spv_emit_shift_left_u32(&c->e, c->t_u32, hi, cu(c, 16)));
        }
    }
    for (u32 m = 0; m < (1u << count); m++)
        if (in->dst < SM86_RZ) wr(c, in->dst + m, out[m]);
}
/* MOVM.16.MT88 (0x23a): the 8x8 matrix of 16-bit elements in the warp's register at 24, transposed into the
   destination, as PTX's movmatrix.sync.aligned.m8n8.trans.b16. Lane l holds row l / 4 at columns 2(l % 4) and
   2(l % 4) + 1, so the transposed pair of lane l, column l / 4 at rows 2(l % 4) and 2(l % 4) + 1, is half (l / 4) % 2
   of lanes 8(l % 4) + (l / 4) / 2 and 8(l % 4) + 4 + (l / 4) / 2 */
static void movm(Ctx* c, const Sm86Insn* in) {
    if (sm86_field(in, 72, 19)) {
        fail(c, "MOVM variant %#x", sm86_field(in, 72, 19));
        return;
    }
    u32 src = rd(c, sm86_field(in, 24, 8)), lane = warp_lane(c);
    u32 g = spv_emit_shift_right_u32(&c->e, c->t_u32, lane, cu(c, 2));
    u32 t8 =
        spv_emit_shift_left_u32(&c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, lane, cu(c, 3)), cu(c, 3));
    u32 from = spv_emit_iadd_u32(&c->e, c->t_u32, t8, spv_emit_shift_right_u32(&c->e, c->t_u32, g, cu(c, 1)));
    u32 shift =
        spv_emit_shift_left_u32(&c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, g, cu(c, 1)), cu(c, 4));
    u32 v0 = warp_read(c, src, from), v1 = warp_read(c, src, spv_emit_iadd_u32(&c->e, c->t_u32, from, cu(c, 4)));
    u32 lo =
        spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, v0, shift), cu(c, 0xffff));
    u32 hi =
        spv_emit_bitwise_and_u32(&c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, v1, shift), cu(c, 0xffff));
    wr(c, in->dst,
       spv_emit_bitwise_or_u32(&c->e, c->t_u32, lo, spv_emit_shift_left_u32(&c->e, c->t_u32, hi, cu(c, 16))));
}
/* the ballot of a predicate over the invocation's 32 lanes, on a subgroup of 32 or 64 */
static u32 ballot32(Ctx* c, u32 pred) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    u32 all = spv_emit_subgroup_ballot(&c->e, c->t_v4u, c->t_u32, pred);
    u32 half = spv_emit_shift_right_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, 5));
    return SPV(&c->e, 77 /* OpVectorExtractDynamic */, c->t_u32, all, half);
}

/* IPA of component k of generic input i: at the centroid (loc 1) or the default place, and with the program's
   multiply by w taken back for a perspective input of frequency 0 */
static u32 ipa_input(Ctx* c, u32 i, u32 k, u32 loc, u32 freq) {
    u32 v = to_f(c, input_load(c, i, k));
    if (loc == 1 && c->in_attr[i] && !(c->centroid_mask & (1u << i))) {
        /* a variable has one location: centroid when any IPA of the input asks for it. An input that no stage
           before writes (in_unfed_mask) is a constant with no variable: a decoration of it named id 0, a module
           that spirv-val and the validation layer refuse */
        spv_emit_decorate(&c->e, c->in_attr[i], 16 /* Centroid */, NULL, 0);
        c->centroid_mask |= 1u << i;
    }
    /* The hardware interpolates attr/w for a perspective input, and the program multiplies by w (the reciprocal of
       a[0x7c]). Vulkan's input is attr itself: times the interpolated 1/w, FragCoord.w */
    if (interp_mode(c, i) == 2 && freq == 0) {
        v = spv_emit_fmul(&c->e, c->t_f32, v, frag_coord(c, 3));
        spv_emit_no_contraction(&c->e, v);
    }
    return v;
}

/* ---- one instruction ---- */

/* the fragment program's outputs from the registers when it ends: the colours of the render targets the SPH's omap
   enables, four registers a target, then the sample mask's register and the depth's after it (Maxwell's layout, as
   emit_flow.c reads it: the depth is the last colour register plus two whether the program writes a sample mask or
   not, as nouveau's nvc0_fp_assign_output_slots has it from Kepler on) */
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
    /* reg: the sample mask, which a single-sampled target ignores */
    if (c->sph[19] & 2u) {
        if (!c->depth) {
            c->depth = spv_emit_frag_depth_output(&c->e, c->t_f32);
            add_interface(c, c->depth);
        }
        spv_emit_store(&c->e, c->depth, to_f(c, rd(c, reg + 1)));
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
/* A system register as a program reads it. A warp has 32 lanes, so its LANEID is below 32, and a program compares it
   with a bit of its ballots. A GPU may run 64 lanes in a stage whose subgroup size can't be asked for (a vertex program
   on RADV), each half a warp of its own here: its lane in that half. One game's culling programs let the highest lane
   of a ballot add to a counter: the upper half's never matched, so no count, and their instances went over the first
   slots of the list. Whole objects came and went, or jumped to where another's should be */
static u32 program_sr(Ctx* c, u32 sr) {
    u32 v = system_value(c, sr);
    return sr == 0 ? spv_emit_bitwise_and_u32(&c->e, c->t_u32, v, cu(c, 31)) : v;
}
/* for (i = start; i < end; i += step) in a structured loop, i in the Function variable counter: loop_begin gives i in
   the loop's body, and loop_end closes it with the labels it set */
typedef struct {
    u32 counter, step, header, cont, merge;
} MeshLoop;
static u32 loop_begin(Ctx* c, MeshLoop* l, u32 start, u32 end) {
    SpvEmitter* e = &c->e;
    u32 body = spv_emit_id(e);
    l->header = spv_emit_id(e), l->cont = spv_emit_id(e), l->merge = spv_emit_id(e);
    spv_emit_store(e, l->counter, start);
    spv_emit_branch(e, l->header);
    spv_emit_label(e, l->header);
    u32 i = spv_emit_load(e, c->t_u32, l->counter);
    u32 more = spv_emit_u_less(e, c->t_bool, i, end);
    spv_emit_loop_merge(e, l->merge, l->cont);
    spv_emit_branch_conditional(e, more, body, l->merge);
    spv_emit_label(e, body);
    return i;
}
static void loop_end(Ctx* c, const MeshLoop* l, u32 i) {
    SpvEmitter* e = &c->e;
    spv_emit_branch(e, l->cont);
    spv_emit_label(e, l->cont);
    spv_emit_store(e, l->counter, spv_emit_iadd_u32(e, c->t_u32, i, l->step));
    spv_emit_branch(e, l->header);
    spv_emit_label(e, l->merge);
}
/* the words of an output map from base on, a vec4 for each generic it names into its Output array (generic[i], made
   here, of count elements) at element, and the position's into position: the words k from first on, word k at
   base + 32 k of the attribute area. A component the map does not name is the 0, 0, 0, 1 of a vertex program's
   output. Returns the words read */
static u32 mesh_outputs(Ctx* c, const u32* sph, u32 base, u32 element, u32 count, int per_primitive, u32* generic) {
    SpvEmitter* e = &c->e;
    u32 k = 0, area = isbe_var(c, &c->isbe_attr, SPV_STORAGE_WORKGROUP, c->isbe_attr_words);
    u32 pointer = spv_emit_type_pointer(e, SPV_STORAGE_OUTPUT, c->t_v4f);
    for (u32 slot = 0; slot < 33; slot++) { /* the position (slot 0), then generic slot - 1 */
        u32 first = slot ? SM86_OMAP_GENERIC + 4 * (slot - 1) : SM86_OMAP_POSITION;
        if (!((sph[first >> 5] >> (first & 31)) & 15)) continue;
        u32 v[4];
        for (u32 comp = 0; comp < 4; comp++)
            if (omap_bit(sph, first + comp)) {
                u32 index = spv_emit_iadd_u32(e, c->t_u32, base, cu(c, 32 * k++));
                v[comp] = to_f(c, isbe_load(c, area, SPV_STORAGE_WORKGROUP, c->isbe_attr_words, index));
            } else
                v[comp] = cf(c, comp == 3 ? 1.0f : 0.0f);
        u32 var;
        if (!slot) {
            /* the depth fix of vertex_depth_fix: z becomes (z + w) / 2 when the push constant's y is not 0 */
            u32 on = spv_emit_fcompare(e, c->t_bool, 5, push_component(c, 1), cf(c, 0.0f)); /* 5: != */
            u32 sum = nocontract(c, spv_emit_fadd(e, c->t_f32, v[2], v[3]));
            v[2] = spv_emit_select(e, c->t_f32, on, nocontract(c, spv_emit_fmul(e, c->t_f32, sum, cf(c, 0.5f))), v[2]);
            if (!c->position) {
                c->position = spv_emit_interface_variable(e, SPV_STORAGE_OUTPUT, c->t_v4f, c->t_u32, count,
                                                          SPV_BUILT_IN_POSITION, 0, 0);
                add_interface(c, c->position);
            }
            var = c->position;
        } else {
            u32 i = slot - 1;
            if (!generic[i]) {
                generic[i] = spv_emit_interface_variable(e, SPV_STORAGE_OUTPUT, c->t_v4f, c->t_u32, count,
                                                         SPV_NOT_BUILTIN, i, 0);
                if (per_primitive) {
                    spv_emit_decorate(e, generic[i], 5271 /* PerPrimitiveEXT */, NULL, 0);
                    spv_emit_flat(e, generic[i]); /* a per-primitive output is not interpolated */
                }
                add_interface(c, generic[i]);
            }
            var = generic[i];
        }
        spv_emit_store(e, SPV(e, SPV_OP_ACCESS_CHAIN, pointer, var, element), spv_emit_vec4_construct(e, c->t_v4f, v));
    }
    return k;
}
/* A task or mesh program's entry point: the counts that the program writes start at 0, then the program's body (the
   function body), a workgroup barrier, and what the hardware does with the ISBE after the program:
   - a task program launches the count of mesh groups at payload byte 4 (x only, as the hardware launches one list
     of groups) with the payload.
   - a mesh program sets its outputs: its most vertices (no program writes a vertex count) and the primitive count at
     map byte 3, then each invocation writes every threads-th vertex and primitive: the position and generics from
     the attribute area, the per-primitive generics (PerPrimitiveEXT) and the triangles' indices from the map.
   Returns the entry point's function */
static u32 mesh_entry(Ctx* c, u32 body) {
    SpvEmitter* e = &c->e;
    const ShMesh* m = &c->si->mesh;
    u32 fn = spv_emit_id(e);
    spv_emit_function_void(e, c->t_void, fn, spv_emit_type_function_void(e, c->t_void));
    spv_emit_label(e, spv_emit_id(e));
    MeshLoop loop = {0};
    if (c->stage == ST_MS) loop.counter = spv_emit_function_variable(e, spv_emit_type_function_pointer(e, c->t_u32));
    loop.step = cu(c, m->threads);
    u32 invocation = builtin_u32(c, &c->local_index, 29); /* LocalInvocationIndex */
    u32 first = spv_emit_i_equal(e, c->t_bool, invocation, c->zero);
    u32 then = spv_emit_id(e), merge = spv_emit_id(e);
    spv_emit_selection_merge(e, merge);
    spv_emit_branch_conditional(e, first, then, merge);
    spv_emit_label(e, then);
    if (c->stage == ST_TS)
        spv_emit_store(e, isbe_ptr(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, cu(c, 1)), c->zero);
    else
        spv_emit_store(e, isbe_ptr(c, map_var(c), SPV_STORAGE_WORKGROUP, cu(c, 3)), c->zero);
    spv_emit_branch(e, merge);
    spv_emit_label(e, merge);
    spv_emit_control_barrier(e, c->t_u32);
    SPV(e, SPV_OP_FUNCTION_CALL, c->t_void, body);
    spv_emit_control_barrier(e, c->t_u32);
    if (c->stage == ST_TS) {
        u32 count = spv_emit_load(e, c->t_u32, isbe_ptr(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, cu(c, 1)));
        SPV_VOID(e, 5294 /* OpEmitMeshTasksEXT */, count, cu(c, 1), cu(c, 1), payload_var(c));
        spv_emit_function_end(e);
        return fn;
    }
    u32 map = map_var(c);
    u32 written = spv_emit_load(e, c->t_u32, isbe_ptr(c, map, SPV_STORAGE_WORKGROUP, cu(c, 3)));
    u32 primitives = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, written, cu(c, m->max_primitives));
    SPV_VOID(e, 5295 /* OpSetMeshOutputsEXT */, cu(c, m->max_vertices), primitives);
    /* the vertices */
    if (c->vertex_words) {
        u32 v = loop_begin(c, &loop, invocation, cu(c, m->max_vertices));
        u32 group = spv_emit_imul_u32(e, c->t_u32, spv_emit_shift_right_u32(e, c->t_u32, v, cu(c, 5)),
                                      cu(c, 32 * c->vertex_words));
        u32 base = spv_emit_iadd_u32(e, c->t_u32, group, spv_emit_bitwise_and_u32(e, c->t_u32, v, cu(c, 31)));
        mesh_outputs(c, c->sph, base, v, m->max_vertices, 0, c->out_attr);
        loop_end(c, &loop, v);
    }
    /* the primitives */
    u32 p = loop_begin(c, &loop, invocation, primitives);
    u32 pointer = spv_emit_type_pointer(e, SPV_STORAGE_OUTPUT, c->t_v3u);
    u32 indices = spv_emit_interface_variable(e, SPV_STORAGE_OUTPUT, c->t_v3u, c->t_u32, m->max_primitives,
                                              5296 /* PrimitiveTriangleIndicesEXT */, 0, 0);
    add_interface(c, indices);
    u32 at = spv_emit_iadd_u32(e, c->t_u32, spv_emit_imul_u32(e, c->t_u32, p, cu(c, 3)), cu(c, 4)), index[3];
    for (u32 k = 0; k < 3; k++)
        index[k] = isbe_load(c, map, SPV_STORAGE_WORKGROUP, c->isbe_map_words,
                             spv_emit_iadd_u32(e, c->t_u32, at, cu(c, k)));
    /* an index at or past the vertex count reads a vertex the program never wrote: some drivers hang on it (the
       Xid 109). Clamp to the last vertex, as the vertex count itself isn't written */
    u32 last_vertex = cu(c, m->max_vertices - 1);
    for (u32 k = 0; k < 3; k++)
        index[k] = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, index[k], last_vertex);
    spv_emit_store(e, SPV(e, SPV_OP_ACCESS_CHAIN, pointer, indices, p),
                   SPV(e, SPV_OP_COMPOSITE_CONSTRUCT, c->t_v3u, index[0], index[1], index[2]));
    if (c->primitive_words) {
        u32 groups = (m->max_vertices + 31) / 32;
        u32 group = spv_emit_imul_u32(e, c->t_u32, spv_emit_shift_right_u32(e, c->t_u32, p, cu(c, 5)),
                                      cu(c, 32 * c->primitive_words));
        u32 base = spv_emit_iadd_u32(e, c->t_u32, group, spv_emit_bitwise_and_u32(e, c->t_u32, p, cu(c, 31)));
        base = spv_emit_iadd_u32(e, c->t_u32, base, cu(c, 32 * c->vertex_words * groups));
        mesh_outputs(c, m->prim_sph, base, p, m->max_primitives, 1, c->prim_attr);
    }
    loop_end(c, &loop, p);
    spv_emit_return(e);
    spv_emit_function_end(e);
    return fn;
}
static int stage_ok(Ctx* c, const Sm86Insn* in) {
    int op = in->op;
    int compute = op == SM86_S2R || op == SM86_S2UR || op == SM86_LDS || op == SM86_STS || op == SM86_STS_NOU ||
                  op == SM86_BAR || op == SM86_ATOMS || op == SM86_ATOMS_NOU || op == SM86_ATOMS_POPC_INC;
    int fragment = op == SM86_IPA || op == SM86_IPA_U || op == SM86_KILL || op == SM86_TMML;
    /* the thread and block ids (TID 0x21-0x23, CTAID 0x25-0x27) are a compute program's. The lane and its masks
       (LANEID 0x00, EQMASK to GEMASK 0x38-0x3c) are every stage's, as a fragment program's LTMASK before a VOTE shows.
       A task or mesh workgroup has thread ids too, but its index comes from the ISBE (workgroup_index) */
    if (compute && (op == SM86_S2R || op == SM86_S2UR)) {
        u32 sr = sm86_field(in, 72, 8);
        compute = sr >= 0x20 && sr <= 0x27;
        if (op == SM86_S2R && sr >= 0x21 && sr <= 0x23 && (c->stage == ST_TS || c->stage == ST_MS)) return 1;
    }
    if (compute && c->stage != ST_CS) return 0;
    if (fragment && c->stage != ST_FS) return 0;
    return 1;
}

/* IADD3 and UIADD3: a + b + c, with the carries out in the predicates p and q and, for .X, two carries in */
static void emit_insn_iadd3(Ctx* c, const Sm86Insn* in, int uniform, u32 p, u32 q, int x) {
    if (p >= SM86_PT && q >= SM86_PT && !x) {
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

static void emit_insn(Ctx* c, const Sm86Insn* in) {
    int op = in->op, uniform = sm86_ops[op].kind == SM86_K_UALU;
    switch (op) {
        case SM86_NOP:
        case SM86_DEPBAR:
        case SM86_BSSY:
        case SM86_BSYNC:
        case SM86_BREAK: /* it takes its lanes out of a convergence barrier and goes on: the BRA after it leaves */
        case SM86_WARPSYNC: return;
        case SM86_BMOV_R:
            /* BMOV.32 R, B reads a convergence barrier, which the structured translation does not keep. Into RZ (a
               BMOV.32.CLEAR RZ, B0 before the BSSY B0 of an NVN vertex program) it changes nothing computed */
            if (in->dst != SM86_RZ) fail(c, "BMOV (to register) into R%u", in->dst);
            return;
        case SM86_BMOV_BI:
        case SM86_BMOV_BL: {
            /* A write of a convergence barrier, whose value the structured translation does not keep (BSSY's writes
               neither), and which no translated instruction reads: BMOV into a register other than RZ is refused.
               The debug variants of the driver's programs start with BMOV.32 B0, 0xffffffff, BMOV.32 B1, B0 to
               BMOV.32 B15, B14 and BMOV.32 B15, 0x0. A state register (28-29 set, for BMOV.32 B, B at 16-23) is
               refused */
            u32 to = op == SM86_BMOV_BI ? sm86_field(in, 24, 8) : sm86_field(in, 16, 8);
            if (to >= 16) fail(c, "%s into state register %u", sm86_ops[op].name, to);
            return;
        }
        case SM86_MOV: wr(c, in->dst, src_bits(c, &in->b)); return;
        case SM86_FADD:
        case SM86_FMUL:
        case SM86_FFMA: {
            /* bit 77 saturate, 78-79 the rounding mode, 80 FTZ, and FMUL's 84-86 the scale 2^(field - 4), which
               nvdisasm names .D4 (2), .D2 (3) and none (4) */
            int on = (int)sm86_field(in, 80, 1);
            u32 mode = sm86_field(in, 78, 2), scale = sm86_field(in, 84, 3), v;
            note_ftz(c, on);
            u32 x = ftz(c, src_f(c, &in->a), on);
            if (op == SM86_FADD)
                v = float_add(c, mode, x, ftz(c, src_f(c, second(in)), on));
            else if (op == SM86_FMUL) {
                if (!scale) {
                    fail(c, "FMUL with scale field 0");
                    return;
                }
                v = float_mul(c, mode, scale, x, ftz(c, src_f(c, &in->b), on));
            } else
                v = float_fma(c, mode, x, ftz(c, src_f(c, &in->b), on), ftz(c, src_f(c, &in->c), on));
            dwr(c, in, to_u(c, saturate(c, ftz(c, v, on), (int)sm86_field(in, 77, 1))));
            return;
        }
        case SM86_FSWZADD: {
            /* a at 24, b at 64, the swizzle at 32-39. Each lane of a quad does the mode in its two bits of the
               swizzle, lane L's at bits 6 - 2L (lane 0 the upper left), as Maxwell's FSWZADD (emit_float.c): a + b
               (0), b - a (1), a - b (2) or b (3). CuAssembler's sm_86 names show the order: 0xcc is ZPPPZPPP, b
               alone in lanes 0 and 2. Bit 77 is NDV, which changes nothing computed. The other bits are refused */
            if (sm86_field(in, 72, 19) & ~(1u << 5)) {
                fail(c, "FSWZADD with modifiers 0x%x", sm86_field(in, 72, 19));
                return;
            }
            u32 swizzle = sm86_field(in, 32, 8);
            u32 a = to_f(c, rd(c, sm86_field(in, 24, 8))), b = to_f(c, rd(c, sm86_field(in, 64, 8)));
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
            u32 lane = spv_emit_bitwise_and_u32(&c->e, c->t_u32, system_value(c, 0), cu(c, 3));
            u32 shift =
                spv_emit_isub_u32(&c->e, c->t_u32, cu(c, 6), spv_emit_shift_left_u32(&c->e, c->t_u32, lane, cu(c, 1)));
            u32 mode = spv_emit_bitwise_and_u32(
                &c->e, c->t_u32, spv_emit_shift_right_u32(&c->e, c->t_u32, cu(c, swizzle), shift), cu(c, 3));
            u32 x = spv_emit_select(&c->e, c->t_f32, spv_emit_i_equal(&c->e, c->t_bool, mode, cu(c, 1)),
                                    spv_emit_fnegate(&c->e, c->t_f32, a), a);
            u32 y = spv_emit_select(&c->e, c->t_f32, spv_emit_i_equal(&c->e, c->t_bool, mode, cu(c, 2)),
                                    spv_emit_fnegate(&c->e, c->t_f32, b), b);
            u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_f32, x, y));
            dwr(c, in,
                to_u(c, spv_emit_select(&c->e, c->t_f32, spv_emit_i_equal(&c->e, c->t_bool, mode, cu(c, 3)), b, sum)));
            return;
        }
        case SM86_FMNMX: {
            u32 min = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            int on = (int)sm86_field(in, 80, 1);
            u32 x = ftz(c, src_f(c, &in->a), on), y = ftz(c, src_f(c, &in->b), on);
            u32 lo = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN, x, y);
            u32 hi = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, y);
            dwr(c, in, to_u(c, spv_emit_select(&c->e, c->t_f32, min, lo, hi)));
            return;
        }
        case SM86_FSET: {
            int on = (int)sm86_field(in, 80, 1);
            u32 cmp = spv_emit_fcompare(&c->e, c->t_bool, sm86_field(in, 76, 4), ftz(c, src_f(c, &in->a), on),
                                        ftz(c, src_f(c, &in->b), on));
            u32 acc = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            cmp = combine(c, sm86_field(in, 74, 2), cmp, acc);
            dwr(c, in, to_u(c, spv_emit_select(&c->e, c->t_f32, cmp, cf(c, 1.0f), cf(c, 0.0f))));
            return;
        }
        case SM86_FSETP:
        case SM86_UISETP:
        case SM86_ISETP: {
            u32 cmp;
            if (op == SM86_FSETP) {
                int on = (int)sm86_field(in, 80, 1);
                cmp = spv_emit_fcompare(&c->e, c->t_bool, sm86_field(in, 76, 4), ftz(c, src_f(c, &in->a), on),
                                        ftz(c, src_f(c, &in->b), on));
            } else {
                cmp = int_compare(c, sm86_field(in, 76, 3), (int)sm86_field(in, 73, 1), src_int(c, &in->a),
                                  src_int(c, &in->b));
                if (sm86_field(in, 72, 1)) { /* .EX: equal high words take the low words' comparison */
                    u32 low = prd(c, sm86_field(in, 68, 3), sm86_field(in, 71, 1), uniform);
                    u32 equal = spv_emit_i_equal(&c->e, c->t_bool, src_int(c, &in->a), src_int(c, &in->b));
                    u32 cmp_op = sm86_field(in, 76, 3);
                    if (cmp_op != 0 && cmp_op != 7) cmp = spv_emit_select(&c->e, c->t_bool, equal, low, cmp);
                }
            }
            u32 acc = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 set_op = sm86_field(in, 74, 2);
            pwr(c, sm86_field(in, 81, 3), combine(c, set_op, cmp, acc), uniform);
            pwr(c, sm86_field(in, 84, 3), combine(c, set_op, spv_emit_bool_not(&c->e, c->t_bool, cmp), acc), uniform);
            return;
        }
        case SM86_HSETP2: {
            /* the halves of a and of b, each pair compared by the operation at 76-79 (spv_emit_fcompare's codes, as
               FSETP's), then each with the predicate at 87-90 by the operation at 69-70: the low halves into the
               predicate at 81-83, the high ones into 84-86. The forms of the 16-bit ALU, as CuAssembler's sm_86
               encodings show them: b a register at 32 (1), two halves at 32-63 (2), a constant buffer (3). a's
               swizzle is at 74-75 and b's at 60-61 (HADD2's codes). FTZ (80) and the other bits of 64-71, which no program
               sets, are refused */
            if (in->form > 3 || sm86_field(in, 80, 1) || sm86_field(in, 64, 5) || sm86_field(in, 71, 1)) {
                fail(c, "HSETP2 of form %u with modifiers 0x%x", in->form, sm86_field(in, 64, 17));
                return;
            }
            const Sm86Src* b = second(in);
            u32 sw[2] = {sm86_field(in, 74, 2), b->kind == SM86_SRC_IMM ? 0 : sm86_field(in, 60, 2)};
            const Sm86Src* src[2] = {&in->a, b};
            u32 v[2];
            for (u32 k = 0; k < 2; k++) {
                if (sw[k] == 1) {
                    fail(c, "HSETP2 with a float operand");
                    return;
                }
                u32 pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, src[k]));
                if (sw[k] >= 2) {
                    u32 h = spv_emit_vec4_extract(&c->e, c->t_f32, pair, sw[k] - 2);
                    pair = spv_emit_vec2_construct(&c->e, c->t_v2f, h, h);
                }
                if (src[k]->kind != SM86_SRC_IMM && src[k]->abs) pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, pair);
                if (src[k]->kind != SM86_SRC_IMM && src[k]->neg) pair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, pair);
                v[k] = pair;
            }
            u32 acc = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), set_op = sm86_field(in, 69, 2);
            for (u32 k = 0; k < 2; k++) {
                u32 cmp = spv_emit_fcompare(&c->e, c->t_bool, sm86_field(in, 76, 4),
                                            spv_emit_vec4_extract(&c->e, c->t_f32, v[0], k),
                                            spv_emit_vec4_extract(&c->e, c->t_f32, v[1], k));
                pwr(c, sm86_field(in, 81 + 3 * k, 3), combine(c, set_op, cmp, acc), 0);
            }
            return;
        }
        case SM86_FSEL:
        case SM86_USEL:
        case SM86_SEL: {
            u32 p = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 x = op == SM86_FSEL ? to_u(c, src_f(c, &in->a)) : src_bits(c, &in->a);
            u32 y = op == SM86_FSEL ? to_u(c, src_f(c, &in->b)) : src_bits(c, &in->b);
            dwr(c, in, spv_emit_select(&c->e, c->t_u32, p, x, y));
            return;
        }
        case SM86_MUFU: {
            /* .F16 (bit 73): the operand is the half bit 60 names, with b's modifiers, and the result the low half */
            int half = (int)sm86_field(in, 73, 1);
            u32 v = 0, fn = sm86_field(in, 74, 6), x;
            if (half && sm86_field(in, 61, 1)) {
                fail(c, "MUFU.F16 with a swizzle");
                return;
            }
            if (half) {
                x = spv_emit_vec4_extract(&c->e, c->t_f32,
                                          SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, &in->b)),
                                          sm86_field(in, 60, 1));
                if (in->b.abs) x = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, x);
                if (in->b.neg) x = spv_emit_fnegate(&c->e, c->t_f32, x);
            } else
                x = src_f(c, &in->b);
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
            dwr(c, in,
                half ? SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                               spv_emit_vec2_construct(&c->e, c->t_v2f, v, cf(c, 0.0f)))
                     : to_u(c, v));
            return;
        }
        case SM86_F2F: {
            /* NAK: log2 of the destination's bytes at 75, of the source's at 84, the rounding at 78 and FTZ at 80. One
               game's programs only narrow a float to the low half, rounding to nearest */
            u32 dsize = sm86_field(in, 75, 2), ssize = sm86_field(in, 84, 2), rnd = sm86_field(in, 78, 2);
            if (dsize != 1 || ssize != 2 || rnd) {
                fail(c, "F2F from %u to %u bytes, rounding %u", 1u << ssize, 1u << dsize, rnd);
                return;
            }
            dwr(c, in,
                SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                        spv_emit_vec2_construct(&c->e, c->t_v2f, src_f(c, &in->b), cf(c, 0.0f))));
            return;
        }
        case SM86_F2I: {
            u32 dsize = sm86_field(in, 75, 2); /* NAK: log2 of the destination's bytes */
            u32 ssize = sm86_field(in, 84, 2);
            if (dsize == 3 || (ssize != 1 && ssize != 2)) {
                fail(c, "F2I of sizes %u, %u", ssize, dsize);
                return;
            }
            /* a half's source: the one bit 60 names, with b's modifiers, as MUFU.F16's */
            u32 from;
            if (ssize == 1) {
                from = spv_emit_vec4_extract(&c->e, c->t_f32,
                                             SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, &in->b)),
                                             sm86_field(in, 60, 1));
                if (in->b.abs) from = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, from);
                if (in->b.neg) from = spv_emit_fnegate(&c->e, c->t_f32, from);
            } else
                from = src_f(c, &in->b);
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            u32 x = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[sm86_field(in, 78, 2)], from);
            int is_signed = (int)sm86_field(in, 72, 1);
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
        case SM86_I2F:
        case SM86_I2FP: {
            u32 src_size = sm86_field(in, 84, 2);
            if (sm86_field(in, 75, 2) != 2 || src_size == 3) {
                fail(c, "%s of sizes %u, %u", sm86_ops[op].name, src_size, sm86_field(in, 75, 2));
                return;
            }
            /* the rounding mode in 78-79: a 32-bit integer is exact in a double. An 8- or 16-bit source (84-85) is
               the byte (.B1 to .B3) or halfword (.H1) that 60-61 select (CuAssembler sm_86) */
            u32 x = src_bits(c, &in->b), mode = sm86_field(in, 78, 2), is_signed = sm86_field(in, 74, 1);
            if (src_size < 2) {
                u32 width = src_size ? 16 : 8, at = cu(c, width * sm86_field(in, 60, 2));
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
        case SM86_FRND: {
            if (sm86_field(in, 75, 2) != 2 || sm86_field(in, 84, 2) != 2) {
                fail(c, "FRND of 64-bit or 16-bit floats");
                return;
            }
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            dwr(c, in,
                to_u(c,
                     spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[sm86_field(in, 78, 2)], src_f(c, &in->b))));
            return;
        }
        case SM86_IADD3:
        case SM86_UIADD3: {
            u32 p = sm86_field(in, 81, 3), q = sm86_field(in, 84, 3);
            int x = (int)sm86_field(in, 74, 1);
            emit_insn_iadd3(c, in, uniform, p, q, x);
            return;
        }
        case SM86_UIMAD:
        case SM86_IMAD: {
            /* .X (bit 74) adds the carry predicate at 87-90, as IADD3.X does */
            u32 v = spv_emit_imul_u32(&c->e, c->t_u32, src_int(c, &in->a), src_int(c, &in->b));
            v = spv_emit_iadd_u32(&c->e, c->t_u32, v, src_int(c, &in->c));
            if (sm86_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_select(&c->e, c->t_u32,
                                                      prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform),
                                                      cu(c, 1), c->zero));
            dwr(c, in, v);
            return;
        }
        case SM86_UIMNMX:
        case SM86_IMNMX: {
            u32 min = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 x = src_bits(c, &in->a), y = src_bits(c, &in->b);
            u32 v;
            if (sm86_field(in, 73, 1)) {
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
        case SM86_FLO:
        case SM86_UFLO: {
            /* NAK's OpFlo: bit 73 signed, 74 the shift amount (31 - the bit) in place of the bit, 63 the source
               inverted. No 1 gives 0xffffffff, as FindUMsb's -1 */
            u32 x = src_bits(c, &in->b);
            if (sm86_field(in, 63, 1)) x = spv_emit_not_u32(&c->e, c->t_u32, x);
            u32 bit = sm86_field(in, 73, 1)
                          ? to_u(c, SPV_EXT(&c->e, c->t_i32, 74 /* FindSMsb */, spv_emit_bitcast(&c->e, c->t_i32, x)))
                          : SPV_EXT(&c->e, c->t_u32, 75 /* FindUMsb */, x);
            if (sm86_field(in, 74, 1))
                bit = spv_emit_select(&c->e, c->t_u32, spv_emit_i_equal(&c->e, c->t_bool, bit, cu(c, 0xffffffffu)), bit,
                                      spv_emit_isub_u32(&c->e, c->t_u32, cu(c, 31), bit));
            dwr(c, in, bit);
            return;
        }
        case SM86_POPC:
        case SM86_UPOPC: {
            u32 x = src_bits(c, &in->b);
            if (sm86_field(in, 63, 1)) x = spv_emit_not_u32(&c->e, c->t_u32, x);
            dwr(c, in, spv_emit_bit_count(&c->e, c->t_u32, x));
            return;
        }
        case SM86_BREV:
        case SM86_UBREV: dwr(c, in, spv_emit_bit_reverse_u32(&c->e, c->t_u32, src_bits(c, &in->b))); return;
        case SM86_R2UR: /* the register is the same in each invocation: its value as it is */
            uwr(c, sm86_field(in, 16, 6), rd(c, sm86_field(in, 24, 8)));
            return;
        case SM86_P2R:
        case SM86_UP2UR: {
            /* P2R Rd, PR, Ra, mask: Ra with the masked bits (at byte 76-77) from P0-P6, PT the bit 7 */
            u32 byte = sm86_field(in, 76, 2), mask = in->b.value, pr = 0;
            if (in->b.kind != SM86_SRC_IMM) {
                fail(c, "%s with a register mask", sm86_ops[op].name);
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
        case SM86_F2FP: {
            /* F2FP.PACK_AB Rd, Ra, Rb: a in the high half, b in the low one, as PTX's cvt.rn.f16x2.f32; bit 76
               BF16 (CuAssembler sm_86). The halves round to nearest */
            if (sm86_field(in, 78, 1)) {
                /* .MERGE_C (NAK's note): b converted in the low half, c's low half (its high one with .H1, bit 72) in
                   the high half, a unused */
                u32 keep = src_bits(c, &in->c);
                if (sm86_field(in, 72, 1)) keep = spv_emit_shift_right_u32(&c->e, c->t_u32, keep, cu(c, 16));
                u32 low = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                                  spv_emit_vec2_construct(&c->e, c->t_v2f, src_f(c, &in->b), cf(c, 0.0f)));
                dwr(c, in,
                    spv_emit_bitwise_or_u32(&c->e, c->t_u32, spv_emit_shift_left_u32(&c->e, c->t_u32, keep, cu(c, 16)),
                                            low));
                return;
            }
            u32 hi = src_f(c, &in->a), lo = src_f(c, second(in));
            u32 v;
            if (sm86_field(in, 76, 1))
                v = spv_emit_bitwise_or_u32(
                    &c->e, c->t_u32, spv_emit_shift_left_u32(&c->e, c->t_u32, bf16(c, hi), cu(c, 16)), bf16(c, lo));
            else
                v = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, spv_emit_vec2_construct(&c->e, c->t_v2f, lo, hi));
            dwr(c, in, v);
            return;
        }
        case SM86_IABS:
            dwr(c, in,
                to_u(c, spv_emit_glsl_unary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_ABS,
                                            spv_emit_bitcast(&c->e, c->t_i32, src_bits(c, &in->b)))));
            return;
        case SM86_ULEA:
        case SM86_LEA: {
            u32 shift = sm86_field(in, 75, 5), p = sm86_field(in, 81, 3);
            int high = (int)sm86_field(in, 80, 1), x = (int)sm86_field(in, 74, 1);
            u32 a = src_bits(c, &in->a), shifted;
            if (high) /* the high word of (c:a) << shift */
                shifted = shift ? spv_emit_bitwise_or_u32(
                                      &c->e, c->t_u32,
                                      spv_emit_shift_left_u32(&c->e, c->t_u32, src_bits(c, &in->c), cu(c, shift)),
                                      spv_emit_shift_right_u32(&c->e, c->t_u32, a, cu(c, 32 - shift)))
                                : src_bits(c, &in->c);
            else
                shifted = spv_emit_shift_left_u32(&c->e, c->t_u32, a, cu(c, shift));
            if (sm86_field(in, 72, 1)) shifted = spv_emit_isub_u32(&c->e, c->t_u32, c->zero, shifted);
            if (p >= SM86_PT && !x) {
                dwr(c, in, spv_emit_iadd_u32(&c->e, c->t_u32, shifted, src_int(c, &in->b)));
                return;
            }
            u32 sum = op2(c, SPV_OP_I_ADD, u64_type(c), to64(c, shifted), to64(c, src_int(c, &in->b)));
            if (x) sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, carry_in(c, 87, in, uniform));
            dwr(c, in, lo32(c, sum));
            pwr(c, p, spv_emit_u_less(&c->e, c->t_bool, c->zero, hi32(c, sum)), uniform);
            return;
        }
        case SM86_ULOP3:
        case SM86_LOP3: {
            if (sm86_field(in, 80, 1) || sm86_field(in, 87, 4) != 0xf) {
                fail(c, "LOP3 with a predicate operand");
                return;
            }
            u32 v = lut3(c, sm86_field(in, 72, 8), c->t_u32, src_bits(c, &in->a), src_bits(c, &in->b),
                         src_bits(c, &in->c), cu(c, 0xffffffffu), c->zero);
            dwr(c, in, v);
            pwr(c, sm86_field(in, 81, 3), spv_emit_i_not_equal(&c->e, c->t_bool, v, c->zero), uniform);
            return;
        }
        case SM86_USHF:
        case SM86_SHF: {
            u32 dtype = sm86_field(in, 73, 2), bits = dtype < 2 ? 64 : 32;
            int right = (int)sm86_field(in, 76, 1), wrap = (int)sm86_field(in, 75, 1),
                high = (int)sm86_field(in, 80, 1);
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
        case SM86_R2P: {
            u32 byte = sm86_field(in, 76, 2); /* nvdisasm's .B1 sets bit 76, .B2 bit 77 */
            u32 mask = in->b.kind == SM86_SRC_IMM ? in->b.value : 0;
            if (in->b.kind != SM86_SRC_IMM) {
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
        case SM86_UPLOP3:
        case SM86_PLOP3: {
            /* bit 67: the third source is a uniform predicate (NAK) */
            int u = op == SM86_UPLOP3; /* all of UPLOP3's predicates are uniform (NAK) */
            u32 x = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), u);
            u32 y = prd(c, sm86_field(in, 77, 3), sm86_field(in, 80, 1), u);
            u32 z = prd(c, sm86_field(in, 68, 3), sm86_field(in, 71, 1), u || (int)sm86_field(in, 67, 1));
            u32 lut0 = sm86_field(in, 64, 3) | sm86_field(in, 72, 5) << 3, lut1 = sm86_field(in, 16, 8);
            u32 p = sm86_field(in, 81, 3), q = sm86_field(in, 84, 3);
            if (p < SM86_PT) pwr(c, p, lut3(c, lut0, c->t_bool, x, y, z, c->true_v, c->false_v), u);
            if (q < SM86_PT) pwr(c, q, lut3(c, lut1, c->t_bool, x, y, z, c->true_v, c->false_v), u);
            return;
        }
        case SM86_CS2R: {
            if (sm86_field(in, 72, 8) != 0xff) {
                fail(c, "CS2R of system register 0x%x", sm86_field(in, 72, 8));
                return;
            }
            wr(c, in->dst, c->zero);
            if (sm86_field(in, 80, 1) && in->dst < SM86_RZ) wr(c, in->dst + 1, c->zero);
            return;
        }
        case SM86_S2R: wr(c, in->dst, program_sr(c, sm86_field(in, 72, 8))); return;
        case SM86_SHFL_II:
        case SM86_SHFL_RI:
        case SM86_SHFL_IR:
        case SM86_SHFL_RR: {
            /* PTX's shfl.sync: c's bits 8-12 split the 32 lanes into segments, its bits 0-4 clamp the lane. Mode
               (58-59) 0 IDX, 1 UP, 2 DOWN, 3 BFLY. The lane is at 53-57 (an immediate) or 32 (a register), c at
               40-52 (an immediate) or 64 (a register, which the translation does not take) */
            if (op == SM86_SHFL_IR || op == SM86_SHFL_RR) {
                fail(c, "SHFL with c in a register");
                return;
            }
            u32 mode = sm86_field(in, 58, 2), cc = sm86_field(in, 40, 13);
            u32 seg = (cc >> 8) & 31, clamp = cc & 31;
            u32 b = op == SM86_SHFL_II ? cu(c, sm86_field(in, 53, 5)) : rd(c, sm86_field(in, 32, 8));
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
            u32 value = rd(c, sm86_field(in, 24, 8));
            u32 got = SPV(&c->e, 345 /* OpGroupNonUniformShuffle */, c->t_u32, cu(c, 3), value, source);
            wr(c, in->dst, spv_emit_select(&c->e, c->t_u32, ok, got, value));
            pwr(c, sm86_field(in, 81, 3), ok, 0);
            return;
        }
        case SM86_VOTE:
        case SM86_UVOTE: {
            /* NAK's OpVote: the ballot of the predicate at 87-90 into the destination, and ALL (0), ANY (1) or EQ
               (2, bits 72-73) of it into the predicate at 81-83. The ballot is of the invocation's 32 lanes */
            u32 pred = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), how = sm86_field(in, 72, 2);
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
            spv_emit_capability(&c->e, 62 /* GroupNonUniformVote */);
            u32 ballot = ballot32(c, pred);
            if (op == SM86_UVOTE)
                uwr(c, sm86_field(in, 16, 6), ballot);
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
            pwr(c, sm86_field(in, 81, 3), vote, op == SM86_UVOTE);
            return;
        }
        case SM86_MATCH: {
            /* NAK's OpMatch: the lanes of the invocation's 32 whose value (a pair with bit 73) is the same. ANY
               (bit 79) gives them; ALL gives all the active lanes when they all are, else 0, and the predicate */
            int any = (int)sm86_field(in, 79, 1), pair = (int)sm86_field(in, 73, 1);
            u32 r = sm86_field(in, 24, 8);
            u32 lo = rd(c, r), hi = pair ? rd(c, r >= SM86_RZ ? r : r + 1) : 0;
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
                pwr(c, sm86_field(in, 81, 3), all, 0);
            }
            return;
        }
        case SM86_ATOMG:
        case SM86_ATOMG_NOU:
        case SM86_RED: {
            /* NAK's OpAtom on global memory: atomic_opcode's operation at 87-90, the data at 32; RED returns nothing */
            u32 how = sm86_field(in, 87, op == SM86_RED ? 3 : 4), type = sm86_field(in, 73, 3);
            u32 opcode = atomic_opcode(type, how);
            if (!opcode) {
                fail(c, "%s of type %u, operation %u", sm86_ops[op].name, type, how);
                return;
            }
            u32 address, buffer = global_address(c, in, op != SM86_ATOMG_NOU, 1, &address);
            if (!buffer) return;
            mark_read(c, c->gkey);
            mark_written(c, c->gkey, address, 4);
            u32 old = spv_emit_atomic_u32(&c->e, c->t_u32, opcode, mem_ptr(c, buffer, address, 0),
                                          rd(c, sm86_field(in, 32, 8)));
            if (op != SM86_RED) wr(c, in->dst, old);
            return;
        }
        case SM86_ATOMS:
        case SM86_ATOMS_NOU: {
            /* the same on shared memory, addressed as an LDS or STS is */
            u32 how = sm86_field(in, 87, 4), type = sm86_field(in, 73, 3), opcode = atomic_opcode(type, how);
            if (!opcode) {
                fail(c, "%s of type %u, operation %u", sm86_ops[op].name, type, how);
                return;
            }
            u32 address = stack_address(c, in, sm86_field(in, 91, 1) ? sm86_field(in, 64, 6) : SM86_URZ);
            shared_var(c);
            wr(c, in->dst,
               spv_emit_atomic_shared_u32(&c->e, c->t_u32, opcode, mem_ptr(c, 0, address, 0),
                                          rd(c, sm86_field(in, 32, 8))));
            return;
        }
        case SM86_ATOMS_POPC_INC: {
            /* adds the lanes that run it, and returns nothing: one each here, where every lane does its own */
            u32 address = stack_address(c, in, SM86_URZ);
            shared_var(c);
            spv_emit_atomic_shared_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_I_ADD, mem_ptr(c, 0, address, 0), cu(c, 1));
            return;
        }
        case SM86_REDUX: {
            /* NAK's OpRedux: of the source over the invocation's 32 lanes, into a uniform register: 78-80 0 and, 1
               or, 2 xor, 3 sum, 4 min, 5 max, bit 73 signed. A clustered reduction of 32 keeps it to those lanes */
            static const u16 ops[6] = {359, 360, 361, 349, 354, 357}; /* BitwiseAnd, Or, Xor, IAdd, UMin, UMax */
            u32 how = sm86_field(in, 78, 3), opcode;
            if (how > 5) {
                fail(c, "REDUX operation %u", how);
                return;
            }
            opcode = ops[how];
            if (sm86_field(in, 73, 1) && how == 4) opcode = 353; /* SMin */
            if (sm86_field(in, 73, 1) && how == 5) opcode = 356; /* SMax */
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC);
            spv_emit_capability(&c->e, 67 /* GroupNonUniformClustered */);
            u32 v = SPV(&c->e, opcode, c->t_u32, cu(c, 3), 3 /* ClusteredReduce */, rd(c, sm86_field(in, 24, 8)),
                        cu(c, 32));
            uwr(c, sm86_field(in, 16, 6), v);
            return;
        }
        case SM86_MEMBAR:
            /* the device scope, for every scope: acquire and release of buffer, workgroup and image memory */
            spv_emit_memory_barrier(&c->e, c->t_u32, 1, 0x8 | 0x40 | 0x100 | 0x800);
            return;
        case SM86_UMOV: uwr(c, in->dst, src_bits(c, &in->b)); return;
        case SM86_ERRBAR: return;
        case SM86_S2UR: uwr(c, sm86_field(in, 16, 6), program_sr(c, sm86_field(in, 72, 8))); return;
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE: {
            /* NAK's OpIMad64: a * b (bit 73 signed) + the pair at c, into the pair at the destination */
            int is_signed = (int)sm86_field(in, 73, 1);
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
            if (in->c.kind == SM86_SRC_IMM || in->c.kind == SM86_SRC_CBUF) {
                fail(c, "%s with c an immediate or a constant", sm86_ops[op].name);
                return;
            }
            chi = in->c.kind == SM86_SRC_UREG ? urd(c, creg >= SM86_URZ ? creg : creg + 1)
                                              : rd(c, creg >= SM86_RZ ? creg : creg + 1);
            u32 sum =
                op2(c, SPV_OP_I_ADD, t64, op2(c, SPV_OP_I_MUL, t64, a64, b64), pair64(c, src_bits(c, &in->c), chi));
            u32 d = in->dst;
            dwr(c, in, lo32(c, sum));
            if (uniform && d < SM86_URZ)
                uwr(c, d + 1, hi32(c, sum));
            else if (!uniform && d < SM86_RZ)
                wr(c, d + 1, hi32(c, sum));
            return;
        }
        case SM86_IMAD_HI: {
            /* the high word of a * b (bit 73 signed), plus c */
            int is_signed = (int)sm86_field(in, 73, 1);
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
            if (sm86_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_select(&c->e, c->t_u32,
                                                      prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), cu(c, 1),
                                                      c->zero));
            dwr(c, in, v);
            return;
        }
        case SM86_SGXT:
        case SM86_USGXT: {
            /* the low b bits (at most 32) of a, sign (bit 73) or zero extended; 0 bits give 0 */
            u32 bits = spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, src_bits(c, &in->b), cu(c, 32));
            u32 a = src_bits(c, &in->a);
            u32 v = sm86_field(in, 73, 1)
                        ? to_u(c, spv_emit_bitfield_sextract(&c->e, c->t_i32, spv_emit_bitcast(&c->e, c->t_i32, a),
                                                             c->zero, bits))
                        : spv_emit_bitfield_uextract(&c->e, c->t_u32, a, c->zero, bits);
            dwr(c, in, spv_emit_select(&c->e, c->t_u32, spv_emit_i_equal(&c->e, c->t_bool, bits, c->zero), c->zero, v));
            return;
        }
        case SM86_PRMT:
        case SM86_UPRMT: {
            /* the index mode (72-74 = 0): output byte k is byte (sel >> 4k) & 7 of c:a, its sign spread with bit 3 */
            if (sm86_field(in, 72, 3)) {
                fail(c, "PRMT mode %u", sm86_field(in, 72, 3));
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
        case SM86_HFMA2_MMA: {
            /* Only a product of RZ: it is a signed zero (the sign of a's and b's negations), and the sum with the
               two halves of c, in floats, is exact. ptxas loads two halves with HFMA2.MMA R, -RZ, RZ, imm, imm. Other
               modifiers (73-91 but b's negation at 75) are refused */
            int zero = (in->a.kind == SM86_SRC_REG && in->a.reg == SM86_RZ) ||
                       (in->b.kind == SM86_SRC_REG && in->b.reg == SM86_RZ);
            if (!zero || (sm86_field(in, 73, 19) & ~(1u << 2))) {
                fail(c, "HFMA2.MMA of a product other than RZ, or with modifiers 0x%x", sm86_field(in, 73, 19));
                return;
            }
            u32 sign = in->a.neg ^ in->b.neg ? 0x80000000u : 0;
            u32 p = spv_emit_vec2_construct(&c->e, c->t_v2f, to_f(c, cu(c, sign)), to_f(c, cu(c, sign)));
            u32 cpair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, &in->c));
            if (in->c.kind == SM86_SRC_REG && in->c.abs) cpair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, cpair);
            if (in->c.kind == SM86_SRC_REG && in->c.neg) cpair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, cpair);
            u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_v2f, p, cpair));
            dwr(c, in, SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, sum));
            return;
        }
        case SM86_HFMA2: {
            /* a * b + c by halves. a as HADD2's. The operand in the register at 64 keeps its swizzle at 81-82 and its
               modifiers at 83 (abs) and 84 (neg), not where a three-source float's are. Bit 79 is .RELU: a result at or
               below zero becomes zero where the predicate at 87 holds, while infinity and NaN stay. Bit 86 is .H0_NH1:
               b's halves swap and the new low one is negated, the second term of a complex product. Bits 78 and 85 are
               refused. A product of two halves is exact in a float of 32 bits */
            int relu = (int)sm86_field(in, 79, 1), h0_nh1 = (int)sm86_field(in, 86, 1);
            if (sm86_field(in, 78, 1) || sm86_field(in, 85, 1) || (relu && sm86_field(in, 77, 1))) {
                fail(c, "HFMA2 with modes 0x%x, 0x%x, saturate %u", sm86_field(in, 78, 2), sm86_field(in, 85, 2),
                     sm86_field(in, 77, 1));
                return;
            }
            int b_at_64 = in->form == 2 || in->form == 3 || in->form == 7;
            const Sm86Src* src[3] = {&in->a, &in->b, &in->c};
            u32 v[3];
            for (u32 k = 0; k < 3; k++) {
                const Sm86Src* s = src[k];
                int at_64 = k && (k == 1) == b_at_64, imm = s->kind == SM86_SRC_IMM;
                u32 sw = imm      ? 0
                         : k == 0 ? sm86_field(in, 74, 2)
                         : at_64  ? sm86_field(in, 81, 2)
                                  : sm86_field(in, 60, 2);
                int abs = imm ? 0 : at_64 ? (int)sm86_field(in, 83, 1) : s->abs;
                int neg = imm ? 0 : at_64 ? (int)sm86_field(in, 84, 1) : s->neg;
                if (sw == 1) {
                    fail(c, "HFMA2 with a float operand");
                    return;
                }
                if (k == 1 && h0_nh1 && (sw || abs)) {
                    fail(c, "HFMA2.H0_NH1 of an operand with a swizzle or abs");
                    return;
                }
                u32 pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, s));
                if (sw >= 2) {
                    u32 h = spv_emit_vec4_extract(&c->e, c->t_f32, pair, sw - 2);
                    pair = spv_emit_vec2_construct(&c->e, c->t_v2f, h, h);
                }
                if (k == 1 && h0_nh1) {
                    u32 low = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 0),
                        high = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 1);
                    pair = spv_emit_vec2_construct(&c->e, c->t_v2f, SPV(&c->e, SPV_OP_F_NEGATE, c->t_f32, high), low);
                }
                if (abs) pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, pair);
                if (neg) pair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, pair);
                v[k] = pair;
            }
            u32 r = nocontract(
                c, op2(c, SPV_OP_F_ADD, c->t_v2f, nocontract(c, spv_emit_fmul(&c->e, c->t_v2f, v[0], v[1])), v[2]));
            if (sm86_field(in, 77, 1))
                r = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_CLAMP, r,
                            spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 0.0f), cf(c, 0.0f)),
                            spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 1.0f), cf(c, 1.0f)));
            if (relu) {
                u32 holds = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), lane[2];
                for (u32 k = 0; k < 2; k++) {
                    /* a comparison NaN fails, and -0 passes on its way to +0 */
                    u32 x = spv_emit_vec4_extract(&c->e, c->t_f32, r, k);
                    u32 below = op2(c, SPV_OP_F_ORD_LESS_THAN_EQUAL, c->t_bool, x, cf(c, 0.0f));
                    lane[k] = spv_emit_select(&c->e, c->t_f32, op2(c, SPV_OP_LOGICAL_AND, c->t_bool, holds, below),
                                              cf(c, 0.0f), x);
                }
                r = spv_emit_vec2_construct(&c->e, c->t_v2f, lane[0], lane[1]);
            }
            dwr(c, in, SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, r));
            return;
        }
        case SM86_HMUL2:
        case SM86_HMNMX2: {
            /* two halves multiplied, or the smaller or larger of each pair by the predicate at 87 (FMNMX's). The
               operands as HADD2's. HMUL2: bit 77 saturate, 76 and 80 the handling of zeros and denormals, which floats
               of 32 bits don't lose here */
            const Sm86Src* b = second(in);
            u32 sw[2] = {sm86_field(in, 74, 2), b->kind == SM86_SRC_IMM ? 0 : sm86_field(in, 60, 2)};
            const Sm86Src* src[2] = {&in->a, b};
            u32 v[2];
            if (op == SM86_HMUL2 && sm86_field(in, 78, 2)) {
                fail(c, "HMUL2 with mode 0x%x", sm86_field(in, 78, 2));
                return;
            }
            for (u32 k = 0; k < 2; k++) {
                if (sw[k] == 1) {
                    fail(c, "%s with a float operand", sm86_ops[op].name);
                    return;
                }
                u32 pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, src_bits(c, src[k]));
                if (sw[k] >= 2) {
                    u32 h = spv_emit_vec4_extract(&c->e, c->t_f32, pair, sw[k] - 2);
                    pair = spv_emit_vec2_construct(&c->e, c->t_v2f, h, h);
                }
                if (src[k]->kind != SM86_SRC_IMM && src[k]->abs) pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, pair);
                if (src[k]->kind != SM86_SRC_IMM && src[k]->neg) pair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, pair);
                v[k] = pair;
            }
            u32 r;
            if (op == SM86_HMUL2) {
                r = nocontract(c, spv_emit_fmul(&c->e, c->t_v2f, v[0], v[1]));
                if (sm86_field(in, 77, 1))
                    r = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_CLAMP, r,
                                spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 0.0f), cf(c, 0.0f)),
                                spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 1.0f), cf(c, 1.0f)));
            } else {
                u32 min = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), lane[2];
                u32 lo = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_MIN, v[0], v[1]);
                u32 hi = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_MAX, v[0], v[1]);
                for (u32 k = 0; k < 2; k++)
                    lane[k] = spv_emit_select(&c->e, c->t_f32, min, spv_emit_vec4_extract(&c->e, c->t_f32, lo, k),
                                              spv_emit_vec4_extract(&c->e, c->t_f32, hi, k));
                r = spv_emit_vec2_construct(&c->e, c->t_v2f, lane[0], lane[1]);
            }
            dwr(c, in, SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, r));
            return;
        }
        case SM86_HADD2: {
            /* two halves added: bit 77 saturate, 78 a float result, 80 FTZ. The swizzles: a's at 74-75, b's at 60-61
               (NAK: 0 none, 2 the low half twice, 3 the high one twice) */
            /* .F32 (bit 78): the low half's sum as a float, as ptxas's HADD2.F32 R, -RZ, R.H0_H0 widens a half */
            int as_f32 = (int)sm86_field(in, 78, 1);
            const Sm86Src* b = second(in);
            u32 sw[2] = {sm86_field(in, 74, 2), b->kind == SM86_SRC_IMM ? 0 : sm86_field(in, 60, 2)};
            const Sm86Src* src[2] = {&in->a, b};
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
                if (src[k]->kind != SM86_SRC_IMM && src[k]->abs) pair = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_F_ABS, pair);
                if (src[k]->kind != SM86_SRC_IMM && src[k]->neg) pair = SPV(&c->e, SPV_OP_F_NEGATE, c->t_v2f, pair);
                v[k] = pair;
            }
            u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_v2f, v[0], v[1]));
            if (sm86_field(in, 77, 1))
                sum = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_CLAMP, sum,
                              spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 0.0f), cf(c, 0.0f)),
                              spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 1.0f), cf(c, 1.0f)));
            dwr(c, in,
                as_f32 ? to_u(c, spv_emit_vec4_extract(&c->e, c->t_f32, sum, 0))
                       : SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, sum));
            return;
        }
        case SM86_ULDC_IDX: {
            /* ULDC UR, c[bank][UR + offset]: the uniform register at 24, the offset 38-53 signed */
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            if (type != 4 && type != 5) {
                fail(c, "ULDC (indexed) of type %u", type);
                return;
            }
            u32 bias = off & 0x8000u ? off | 0xffff0000u : off, d = sm86_field(in, 16, 6);
            u32 index = urd(c, sm86_field(in, 24, 6));
            uwr(c, d, cbuf_load(c, bank, bias, index));
            if (type == 5 && d < SM86_URZ) uwr(c, d + 1, cbuf_load(c, bank, bias + 4, index));
            return;
        }
        case SM86_ULDC: {
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            if (sm86_field(in, 91, 1) || (type != 4 && type != 5)) {
                fail(c, "ULDC of type %u", type);
                return;
            }
            u32 d = sm86_field(in, 16, 6);
            uwr(c, d, cbuf_load(c, bank, off, 0));
            if (type == 5 && d < SM86_URZ) uwr(c, d + 1, cbuf_load(c, bank, off + 4, 0));
            return;
        }
        case SM86_LDC: {
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            u32 r = sm86_field(in, 24, 8);
            if (sm86_field(in, 78, 2) || type > 5) {
                fail(c, "LDC of type %u, mode %u", type, sm86_field(in, 78, 2));
                return;
            }
            u32 index = r < SM86_RZ ? rd(c, r) : 0;
            if (c->handle && c->handle[c->cur].var) { /* the element of the table of handles this reads */
                const TexHandle* h = &c->handle[c->cur];
                u32 at = spv_emit_isub_u32(&c->e, c->t_u32, spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off)),
                                           cu(c, h->first));
                spv_emit_store(&c->e, h->var, op2(c, SPV_OP_U_DIV, c->t_u32, at, cu(c, h->stride)));
            }
            if (type < 4) {
                /* NAK's MemType U8 (0), I8 (1), U16 (2), I16 (3): a byte or a halfword of the word at the address,
                   zero or sign extended, as mem_load takes one from memory */
                u32 at = index ? spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off)) : cu(c, off);
                u32 shift = spv_emit_shift_left_u32(
                    &c->e, c->t_u32, spv_emit_bitwise_and_u32(&c->e, c->t_u32, at, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
                u32 word = cbuf_load(c, bank, off, index), bits = cu(c, type < 2 ? 8 : 16);
                wr(c, in->dst,
                   type & 1 ? to_u(c, spv_emit_bitfield_sextract(&c->e, c->t_i32,
                                                                 spv_emit_bitcast(&c->e, c->t_i32, word), shift, bits))
                            : spv_emit_bitfield_uextract(&c->e, c->t_u32, word, shift, bits));
                return;
            }
            wr(c, in->dst, cbuf_load(c, bank, off, index));
            if (type == 5 && in->dst < SM86_RZ) wr(c, in->dst + 1, cbuf_load(c, bank, off + 4, index));
            return;
        }
        case SM86_LDG:
        case SM86_STG:
        case SM86_LDG_NOU:
        case SM86_STG_NOU: {
            int load = op == SM86_LDG || op == SM86_LDG_NOU;
            u32 from = c->addr_key[c->n + 1 + c->cur];
            if (!c->addr_key[c->cur] && (from == SM86_LDG || from == SM86_LDG_NOU || from == SM86_LDS ||
                                         from == SM86_LDL)) { /* a pointer from memory */
                global_dynamic(c, in, op == SM86_LDG || op == SM86_STG, load);
                return;
            }
            u32 address, buffer = global_address(c, in, op == SM86_LDG || op == SM86_STG, !load, &address);
            if (!buffer) return;
            u32 type = sm86_field(in, 73, 3);
            if (load)
                mem_load(c, buffer, address, type, in->dst, 0);
            else {
                mark_written(c, c->gkey, address, type < 2 ? 1 : type < 4 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 16);
                mem_store(c, buffer, address, type, sm86_field(in, 32, 8));
            }
            return;
        }
        case SM86_LDS:
        case SM86_STS:
        case SM86_STS_NOU:
        case SM86_LDL:
        case SM86_STL:
        case SM86_STL_NOU: {
            /* The uniform register is there with bit 91 alone, as CuAssembler's sm_86 encodings show: at 32 for a load,
               at 64 for a store, whose data is at 32 */
            int load = op == SM86_LDS || op == SM86_LDL;
            u32 ur = sm86_field(in, 91, 1) ? sm86_field(in, load ? 32 : 64, 6) : SM86_URZ;
            u32 address = stack_address(c, in, ur);
            u32 buffer = op == SM86_LDL || op == SM86_STL || op == SM86_STL_NOU ? MEM_LOCAL : 0;
            if (!buffer) shared_var(c);
            if (load)
                mem_load(c, buffer, address, sm86_field(in, 73, 3), in->dst, 0);
            else
                mem_store(c, buffer, address, sm86_field(in, 73, 3), sm86_field(in, 32, 8));
            return;
        }
        case SM86_ALD:
        case SM86_AST: {
            u32 addr = sm86_field(in, 40, 10), comps = sm86_field(in, 74, 2) + 1;
            u32 vtx = sm86_field(in, op == SM86_ALD ? 32 : 64, 8);
            if (sm86_field(in, 24, 8) != SM86_RZ || vtx != SM86_RZ || sm86_field(in, 76, 2) ||
                (op == SM86_ALD && sm86_field(in, 79, 1))) {
                fail(c, "%s indexed, of a patch or of an output", sm86_ops[op].name);
                return;
            }
            for (u32 k = 0; k < comps; k++) {
                if (op == SM86_ALD) {
                    if (in->dst < SM86_RZ) wr(c, in->dst + k, attr_load(c, addr + 4 * k));
                } else {
                    u32 data = sm86_field(in, 32, 8);
                    attr_store(c, addr + 4 * k, rd(c, data >= SM86_RZ ? data : data + k));
                }
            }
            return;
        }
        case SM86_IPA: {
            u32 addr = sm86_field(in, 64, 8) * 4;
            if (shader_legacy_group(addr) >= 0) addr = shader_legacy_address(addr);
            /* bits 76-77, NAK's InterpLoc: 0 the default, 1 the centroid, 2 at an offset */
            u32 loc = sm86_field(in, 76, 2);
            if (sm86_field(in, 32, 8) != SM86_RZ || sm86_field(in, 24, 8) != 0 || sm86_field(in, 78, 2) > 1 ||
                loc > 1) {
                fail(c, "IPA indexed, at an offset or of frequency %u", sm86_field(in, 78, 2));
                return;
            }
            u32 v;
            if (addr >= 0x70 && addr < 0x80)
                v = frag_coord(c, (addr >> 2) & 3);
            else if (addr >= 0x80 && addr < 0x280)
                v = ipa_input(c, (addr - 0x80) >> 4, (addr >> 2) & 3, loc, sm86_field(in, 78, 2));
            else if (addr == 0x3fc) {
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
        case SM86_IPA_U: {
            /* IPA R, a[UR + addr]: the form with bit 91 and the uniform register at 24, which ptxas gives an input
               array's index. The index is the same in every lane: the generic input whose address the sum is, of
               those the SPH's imap reads, picked by its address (0 for none). The rest is IPA's */
            u32 loc = sm86_field(in, 76, 2), freq = sm86_field(in, 78, 2);
            if (!sm86_field(in, 91, 1) || sm86_field(in, 32, 8) != SM86_RZ || freq > 1 || loc > 1) {
                fail(c, "IPA with a uniform index, at an offset or of frequency %u", freq);
                return;
            }
            u32 at =
                spv_emit_iadd_u32(&c->e, c->t_u32, urd(c, sm86_field(in, 24, 6)), cu(c, sm86_field(in, 64, 8) * 4));
            u32 v = c->zero;
            for (u32 i = 0; i < 32; i++)
                for (u32 k = 0; k < 4; k++)
                    if ((c->sph[6 + i / 4] >> ((i % 4) * 8 + k * 2)) & 3)
                        v = spv_emit_select(&c->e, c->t_u32,
                                            spv_emit_i_equal(&c->e, c->t_bool, at, cu(c, 0x80 + 16 * i + 4 * k)),
                                            to_u(c, ipa_input(c, i, k, loc, freq)), v);
            wr(c, in->dst, v);
            return;
        }
        case SM86_TEX:
        case SM86_TEX_B:
        case SM86_TLD4:
        case SM86_TMML: texture(c, in); return;
        case SM86_TXD: texture_grad(c, in); return;
        case SM86_TLD: texel_fetch(c, in); return;
        case SM86_TXQ: texture_query(c, in); return;
        case SM86_SULD_P:
        case SM86_SUST_P:
        case SM86_SURED:
        case SM86_SUATOM: surface(c, in); return;
        case SM86_HMMA: hmma(c, in); return;
        case SM86_LDSM: ldsm(c, in); return;
        case SM86_MOVM: movm(c, in); return;
        case SM86_KILL:
            spv_emit_capability(&c->e, SPV_CAPABILITY_DEMOTE_TO_HELPER_INVOCATION);
            spv_emit_demote(&c->e);
            return;
        case SM86_ISBERD:
        case SM86_ISBEWR: isbe(c, in); return;
        case SM86_BAR:
            if (sm86_field(in, 77, 2) || in->hi & 0xfffffffull) {
                fail(c, "BAR other than BAR.SYNC 0");
                return;
            }
            spv_emit_control_barrier(&c->e, c->t_u32);
            return;
        default: fail(c, "%s", sm86_ops[op].name); return;
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

/* BRX R, offset: to the byte R + offset + the next instruction's, where the offset is in 4-byte units in bits 34 to
   81, signed, as BRA's (sm86_branch_target). ptxas writes minus the next instruction's address, so that R is the
   target's own: read from bit 33, one game's two BRX each went to their own index plus one less than the table's
   targets, and back to the program's start. The constant added to R, as a u32 */
static u32 brx_bias(const Sm86Insn* in, u32 i) {
    u64 v = (in->lo >> 34) | (in->hi & 0x3ffffull) << 30; /* 48 bits */
    if (v >> 47) v |= ~0ull << 48;
    return (u32)(v * 4 + ((u64)i + 1) * 16);
}
/* the instruction before k, back to the last branch, that writes register r: its index, or -1 */
static int writer_before(Ctx* c, u32 k, u32 r) {
    for (u32 j = k; j-- > 0 && j + 16 > k;) {
        const Sm86Insn* w = &c->in[j];
        if (w->op == SM86_BRA || w->op == SM86_EXIT || is_brx(w->op)) return -1;
        if (sm86_ops[w->op].kind != SM86_K_UALU && w->dst == r) return (int)j;
    }
    return -1;
}
/* The entries of the jump table that the LDC at k indexes with register r, as ptxas bounds a switch's index: an
   unsigned IMNMX with an immediate, then an IMAD by 4 or a 32-bit SHF.L by 2 into r (index = min(x, last) * 4). 0 when
   it isn't that */
static u32 jump_table_entries(Ctx* c, u32 k, u32 r) {
    int m = writer_before(c, k, r);
    if (m < 0) return 0;
    const Sm86Insn* mul = &c->in[m];
    int times_4 = (mul->op == SM86_IMAD && !sm86_field(mul, 74, 1) && mul->b.value == 4) ||
                  (mul->op == SM86_SHF && !sm86_field(mul, 76, 1) && !sm86_field(mul, 80, 1) &&
                   sm86_field(mul, 73, 2) >= 2 && mul->b.value == 2);
    if (!times_4 || mul->pred != SM86_PT || mul->pred_not || mul->a.kind != SM86_SRC_REG ||
        mul->b.kind != SM86_SRC_IMM || mul->c.kind != SM86_SRC_REG || mul->c.reg != SM86_RZ)
        return 0;
    int b = writer_before(c, (u32)m, mul->a.reg);
    if (b < 0) return 0;
    const Sm86Insn* bound = &c->in[b];
    if (bound->op != SM86_IMNMX || bound->pred != SM86_PT || bound->pred_not || sm86_field(bound, 73, 1) ||
        sm86_field(bound, 87, 3) != SM86_PT || sm86_field(bound, 90, 1) || bound->b.kind != SM86_SRC_IMM ||
        bound->b.value >= 64)
        return 0;
    return bound->b.value + 1;
}
/* whether the instruction writes uniform register r: the uniform ALU's, and the loads and moves into one */
static int writes_uniform(const Sm86Insn* w, u32 r) {
    if (w->op == SM86_UISETP) return 0; /* it writes predicates */
    if (sm86_ops[w->op].kind == SM86_K_UALU) return w->dst == r;
    return (w->op == SM86_ULDC || w->op == SM86_ULDC_IDX || w->op == SM86_ULDCG || w->op == SM86_S2UR ||
            w->op == SM86_R2UR || w->op == SM86_UVOTE) &&
           sm86_field(w, 16, 6) == r;
}
/* the instruction before k, back to the last branch, that writes uniform register r: its index, or -1 */
static int uniform_writer_before(Ctx* c, u32 k, u32 r) {
    for (u32 j = k; j-- > 0 && j + 16 > k;) {
        const Sm86Insn* w = &c->in[j];
        if (w->op == SM86_BRA || w->op == SM86_EXIT || is_brx(w->op)) return -1;
        if (writes_uniform(w, r)) return (int)j;
    }
    return -1;
}
/* The entries of the table that the ULDC.IDX at k reads with uniform register r, as ptxas bounds a BRXU's index on
   the uniform datapath: UISETP.LT.U32 UP, UR, imm, then USEL UR', UR, imm, UP (the smaller of the two), then
   USHF.L.U32 by 2 into r (index = min(x, last) * 4). 0 when it isn't that */
static u32 uniform_table_entries(Ctx* c, u32 k, u32 r) {
    int m = uniform_writer_before(c, k, r);
    if (m < 0) return 0;
    const Sm86Insn* shf = &c->in[m];
    if (shf->op != SM86_USHF || shf->pred != SM86_PT || shf->pred_not || shf->b.kind != SM86_SRC_IMM ||
        shf->b.value != 2 || shf->c.reg != SM86_URZ || sm86_field(shf, 76, 1) || sm86_field(shf, 80, 1) ||
        sm86_field(shf, 73, 2) < 2)
        return 0;
    int s = uniform_writer_before(c, (u32)m, shf->a.reg);
    if (s < 0) return 0;
    const Sm86Insn* sel = &c->in[s];
    u32 up = sm86_field(sel, 87, 3);
    if (sel->op != SM86_USEL || sel->pred != SM86_PT || sel->pred_not || sel->b.kind != SM86_SRC_IMM ||
        sel->b.value >= 64 || sm86_field(sel, 90, 1) || up >= SM86_PT)
        return 0;
    u32 x = sel->a.reg, last = sel->b.value;
    for (u32 j = (u32)s; j-- > 0 && j + 16 > (u32)s;) {
        const Sm86Insn* w = &c->in[j];
        if (w->op == SM86_BRA || w->op == SM86_EXIT || is_brx(w->op) || writes_uniform(w, x)) return 0;
        if (w->op != SM86_UISETP || sm86_field(w, 81, 3) != up) continue;
        int lt = sm86_field(w, 76, 3) == 1 && !sm86_field(w, 73, 1) && !sm86_field(w, 72, 1);
        int alone = sm86_field(w, 87, 3) == SM86_PT && !sm86_field(w, 90, 1) && sm86_field(w, 74, 2) == 0;
        return w->pred == SM86_PT && !w->pred_not && lt && alone && w->a.reg == x && w->b.kind == SM86_SRC_IMM &&
                       w->b.value == last
                   ? last + 1
                   : 0;
    }
    return 0;
}
/* The targets of the BRX at i: the jump table of the LDC before it that writes its register, read through
   g_shader_cb_read as spirv_translate.c reads Maxwell's: as many entries as ptxas's bound on the index says, else up
   to the first word that is no target. Two tables one after the other were read as one, and gave a BRX the other's
   targets as its own. The count, or 0 */
static u32 brx_targets(Ctx* c, u32 i, u32* targets, u32 max) {
    const Sm86Insn* in = &c->in[i];
    int uniform = in->op == SM86_BRXU;
    u32 reg = sm86_field(in, 24, uniform ? 6 : 8), bias = brx_bias(in, i);
    if (!(in->pred == SM86_PT && !in->pred_not)) {
        fail(c, "BRX on a predicate");
        return 0;
    }
    if (!g_shader_cb_read) {
        fail(c, "BRX without a reader of constant buffers");
        return 0;
    }
    for (u32 k = i; k-- > 0 && k + 32 > i;) {
        const Sm86Insn* w = &c->in[k];
        if (w->op == SM86_BRA || w->op == SM86_EXIT || is_brx(w->op)) break;
        u32 entries;
        if (uniform) { /* BRXU: the ULDC.IDX that writes its uniform register */
            if (!writes_uniform(w, reg)) continue;
            if (w->op != SM86_ULDC_IDX || sm86_field(w, 73, 3) != 4) break;
            entries = uniform_table_entries(c, k, sm86_field(w, 24, 6));
        } else {
            if (w->dst != reg || sm86_ops[w->op].kind == SM86_K_UALU) continue;
            if (w->op != SM86_LDC || sm86_field(w, 73, 3) != 4 || sm86_field(w, 78, 2)) break;
            entries = jump_table_entries(c, k, sm86_field(w, 24, 8));
        }
        u32 bank = sm86_field(w, 54, 5), off = sm86_field(w, 38, 16), count = 0;
        if (entries > max) {
            fail(c, "BRX with a jump table of %u entries", entries);
            return 0;
        }
        for (; count < (entries ? entries : max); count++) {
            u32 entry;
            if (!g_shader_cb_read(bank, off + 4 * count, &entry)) break;
            u32 at = entry + bias;
            if ((at & 15) || at / 16 >= c->n) break;
            targets[count] = at / 16;
        }
        if (entries && count < entries) {
            fail(c, "BRX whose jump table in c[%u][0x%x] has %u targets of its %u entries", bank, off, count, entries);
            return 0;
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
        const Sm86Insn* in = &c->in[i];
        int never = in->pred == SM86_PT && in->pred_not;
        if (in->op == SM86_BRA && !never) {
            u32 t = sm86_branch_target(in, i);
            if (t >= n) {
                fail(c, "branch out of the program");
                break;
            }
            leader[t] = 1;
            leader[i + 1] = 1;
        } else if (is_brx(in->op) && !never) {
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
        } else if ((in->op == SM86_EXIT && in->pred == SM86_PT && !in->pred_not) || in->op == SM86_RET)
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
            const Sm86Insn* in = &c->in[last];
            int always = in->pred == SM86_PT && !in->pred_not, never = in->pred == SM86_PT && in->pred_not;
            sw_first[b] = nsw;
            if (in->op == SM86_BRA && !never) {
                succ[2 * b] = block_of[sm86_branch_target(in, last)];
                if (always || succ[2 * b] == b + 1) /* as terminators() takes a branch to the next block */
                    term[b] = SHT_GOTO;
                else if (b + 1 < nb) {
                    term[b] = SHT_COND;
                    succ[2 * b + 1] = b + 1;
                } else
                    term[b] = SHT_GOTO; /* a conditional branch off the end: the program can't run off it */
            } else if (is_brx(in->op) && case_of[last]) {
                term[b] = SHT_SWITCH;
                for (u32 k = 0; k < case_count[last]; k++) sw_targets[nsw++] = block_of[cases[case_of[last] - 1 + k]];
            } else if ((in->op == SM86_EXIT && always) || in->op == SM86_RET)
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

/* a BRA from instruction ip to target, on the guard of in (sm86_set_rel_offset's field, bits 34-81) */
static Sm86Insn make_bra(const Sm86Insn* in, u32 ip, u32 target) {
    u64 field = (((u64)target - (u64)ip - 1) * 4) & ((1ull << 48) - 1);
    u64 lo = 0x947u | (u64)in->pred << 12 | (u64)in->pred_not << 15 | field << 34;
    u64 hi = field >> 30 | 7ull << 23 | 7ull << 46 | 7ull << 49; /* condition PT, no barriers */
    Sm86Insn out;
    sm86_decode(lo, hi, &out);
    return out;
}
/* CALL.REL and RET.REL inlined: each CALL becomes a BRA to a copy of its function (from its target up to the BRA
   to itself that ends it), whose RETs become BRAs back to the instruction after the CALL. ptxas's RET returns
   there, through the address that the caller writes into the RET's register. The copies go after the program, and
   the function's own code stays where nothing reaches it. Returns the new instructions (malloc'd) and their count,
   or NULL */
static Sm86Insn* inline_calls(Ctx* c, Sm86Insn* insn, u32* count) {
    u32 n = *count, cap = n * 2 + 64;
    Sm86Insn* out = malloc((size_t)cap * sizeof *out);
    if (!out) return NULL;
    memcpy(out, insn, (size_t)n * sizeof *out);
    for (u32 i = 0, copies = 0; i < n; i++) {
        if (out[i].op != SM86_CALL) continue;
        u32 entry = sm86_branch_target(&out[i], i), end = entry;
        while (end < n && !(out[end].op == SM86_BRA && out[end].pred == SM86_PT && !out[end].pred_not &&
                            sm86_branch_target(&out[end], end) == end))
            end++;
        if (entry >= *count || end >= *count || ++copies > 64) {
            fail(c, "CALL to 0x%x that the inlining can't take", entry * 16);
            free(out);
            return NULL;
        }
        /* ptxas also leaves a loop with @P CALL.REL.NOINC to the code after it, which no RET ends: a branch */
        int returns = 0;
        for (u32 k = entry; k <= end; k++) returns |= insn[k].op == SM86_RET;
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
            Sm86Insn* grown = realloc(out, (size_t)cap * sizeof *out);
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
            const Sm86Insn* in = &out[n + k];
            if (in->op == SM86_CALL) {
                /* the target of a CALL in the copy, relative, now points into the original: keep it so */
                u32 t = sm86_branch_target(in, entry + k);
                Sm86Insn call = make_bra(in, n + k, t);
                call.op = SM86_CALL;
                out[n + k] = call;
            } else if (in->op == SM86_RET)
                out[n + k] = make_bra(in, n + k, i + 1);
            else if (in->op == SM86_BRA || in->op == SM86_BSSY) {
                /* a branch out of the copied function keeps its target in the original */
                u32 t = sm86_branch_target(in, entry + k);
                if (t < entry || t > end) {
                    Sm86Insn far = make_bra(in, n + k, t);
                    if (in->op == SM86_BSSY) far = *in; /* BSSY's target only names a convergence point */
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

/* the constant an operand holds at instruction at */
static int src_const(Ctx* c, u32 at, const Sm86Src* s, u32* value) {
    if (s->kind == SM86_SRC_IMM) {
        *value = s->value;
        return 1;
    }
    return s->kind == SM86_SRC_REG && reg_const_at(c, at, s->reg, value);
}
/* The handle of each TEX.B is in a register. A MOV or an LDC of a constant buffer word gives a slot as any other. An
   LDC whose address has a register in it picks from a table: one game's lights keep 32 records each, in a constant
   buffer, and a program finds the lights of a pixel with FLO on a mask, then an IMAD of that index by the record's
   size makes the LDC's address, c[bank][x * stride + base + offset]. So the texture is chosen for each pixel, from
   an array, and the table's elements are stride bytes apart, whatever the stride (they aren't a power of two) */
static void prepare_handles(Ctx* c) {
    for (u32 i = 0; i < c->n && !c->failed; i++) {
        const Sm86Insn* in = &c->in[i];
        if (in->op != SM86_TEX_B) continue;
        u32 type, array, coords, regs[8], lod = sm86_field(in, 87, 3);
        if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) {
            fail(c, "TEX.B dimension 6");
            return;
        }
        u32 shadow = sm86_field(in, 78, 1), n0 = array + coords;
        u32 n1 = 1 + (lod == 2 || lod == 3) + sm86_field(in, 76, 1) + shadow;
        if (!tex_operands(in, n0, n1, regs)) {
            fail(c, "TEX.B with %u operands in the scalar form", n0 + n1);
            return;
        }
        if (!c->handle) c->handle = calloc((size_t)c->n + 1, sizeof *c->handle);
        if (!c->handle) {
            fail(c, "out of memory");
            return;
        }
        int k = reaching_write(c, i, regs[n0]);
        for (u32 depth = 0; k >= 0 && depth < 8 && c->in[k].op == SM86_MOV && c->in[k].b.kind == SM86_SRC_REG; depth++)
            k = reaching_write(c, (u32)k, c->in[k].b.reg);
        const Sm86Insn* w = k >= 0 ? &c->in[k] : NULL;
        int slot = -1;
        if (!w) {
            fail(c, "TEX.B whose handle R%u has no one writer", regs[n0]);
        } else if (w->op == SM86_MOV && w->b.kind == SM86_SRC_CBUF && !w->b.bindless) {
            slot = tex_slot(c, w->b.bank, w->b.value, type, array, shadow, 0);
        } else if (w->op == SM86_LDC && sm86_field(w, 73, 3) == 4 && !sm86_field(w, 78, 2)) {
            u32 bank = sm86_field(w, 54, 5), off = sm86_field(w, 38, 16), index = sm86_field(w, 24, 8);
            if (index >= SM86_RZ) {
                slot = tex_slot(c, bank, off, type, array, shadow, 0);
            } else {
                int m = reaching_write(c, (u32)k, index);
                const Sm86Insn* mul = m >= 0 ? &c->in[m] : NULL;
                u32 stride = 0, base = 0, count = SNX_TEX_ARRAY;
                if (!g_shader_texture_arrays || c->stage == ST_CS) {
                    fail(c, "TEX.B of a table of handles, without texture arrays");
                } else if (!mul || mul->op != SM86_IMAD || sm86_field(mul, 74, 1) ||
                           !src_const(c, (u32)m, &mul->b, &stride) || !src_const(c, (u32)m, &mul->c, &base) ||
                           !stride || stride > 0xffff) {
                    fail(c, "TEX.B of a table of handles, indexed by %s",
                         mul ? sm86_ops[mul->op].name : "several writes");
                } else {
                    /* the index of a FLO on a 32-bit mask */
                    int x = mul->a.kind == SM86_SRC_REG ? reaching_write(c, (u32)m, mul->a.reg) : -1;
                    if (x >= 0 && c->in[x].op == SM86_FLO) count = 32;
                    slot = tex_slot_indexed(c, bank, base + off, type, array, shadow, stride, count);
                    if (slot >= 0) {
                        TexHandle* h = &c->handle[k];
                        if (!h->var)
                            h->var = spv_emit_function_variable_init(
                                &c->e, spv_emit_type_function_pointer(&c->e, c->t_u32), c->zero);
                        h->first = base + off;
                        h->stride = stride;
                        c->handle[i].ldc = (u32)k + 1;
                    }
                }
            }
        } else {
            fail(c, "TEX.B whose handle comes from %s", sm86_ops[w->op].name);
        }
        if (slot >= 0) c->handle[i].slot = (u32)slot + 1;
    }
}

/* the highest register an instruction's fields can name, of the 8-bit register fields */
static u32 max_register(const Sm86Insn* in) {
    static const u8 fields[] = {16, 24, 32, 64};
    u32 top = 0;
    for (u32 k = 0; k < sizeof fields; k++) {
        u32 r = sm86_field(in, fields[k], 8);
        if (r < SM86_RZ && r > top) top = r;
    }
    return top;
}

/* a task or mesh program's key (ShaderInfo.mesh) checked against the limits of VK_EXT_mesh_shader and the facts of
   the game's programs, and the sizes of its ISBE arrays. A mesh program's out_mask gets its generic outputs, and its
   prim_mask the per-primitive ones */
static void mesh_layout(Ctx* c) {
    const ShMesh* m = &c->si->mesh;
    if (!m->threads || m->threads > 32) {
        fail(c, "a task or mesh workgroup of %u invocations, not one warp", m->threads);
        return;
    }
    if (c->stage == ST_TS || m->has_task) {
        /* the launch count at byte 4, and 16 KiB, the least maxTaskPayloadSize */
        if (m->payload < 8 || m->payload > 16384 || (m->payload & 3)) {
            fail(c, "a task payload of 0x%x bytes", m->payload);
            return;
        }
        c->payload_words = m->payload / 4u;
    }
    if (c->stage == ST_TS) return;
    if (m->primitive_vertices != 3) {
        fail(c, "a mesh program's primitives of %u vertices", m->primitive_vertices);
        return;
    }
    if (!m->max_vertices || m->max_vertices > 256 || !m->max_primitives || m->max_primitives > 256) {
        fail(c, "a mesh program of %u vertices and %u primitives", m->max_vertices, m->max_primitives);
        return;
    }
    /* the per-primitive outputs' header is a geometry program's (shader type 4) */
    if (m->prim_sph[0] && (m->prim_sph[0] & 31) != 1) {
        fail(c, "a per-primitive SPH %08x", m->prim_sph[0]);
        return;
    }
    if (m->prim_sph[0] && (m->prim_sph[0] >> 10 & 15) != 4) {
        fail(c, "a per-primitive SPH of shader type %u", m->prim_sph[0] >> 10 & 15);
        return;
    }
    c->vertex_words = omap_words(c, c->sph, 1, "per-vertex");
    c->primitive_words = m->prim_sph[0] ? omap_words(c, m->prim_sph, 0, "per-primitive") : 0;
    u32 vertex = sm86_omap_generics(c->sph), primitive = sm86_mesh_primitive_mask(m);
    if (vertex & primitive) fail(c, "generics 0x%x both per vertex and per primitive", vertex & primitive);
    c->si->out_mask = vertex | primitive;
    c->si->prim_mask = primitive;
    c->isbe_attr_words =
        32 * (c->vertex_words * ((m->max_vertices + 31u) / 32) + c->primitive_words * ((m->max_primitives + 31u) / 32));
    c->isbe_map_words = 4 + 3u * m->max_primitives;
}

u32* sm86_to_spirv(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count) {
    if (!code || !si || !word_count || stage < ST_VS || stage > ST_MS || bytes < 16) return NULL;
    if (stage != ST_CS && !sph) return NULL;
    if (stage == ST_TCS && sm86_tcs_empty(sph[2])) {
        si->unsupported = 1;
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "a tessellation control program with no invocation (the passthrough takes its place)");
        return NULL;
    }
    if (stage != ST_VS && stage != ST_FS && stage != ST_CS && stage != ST_TS && stage != ST_MS) {
        si->unsupported++;
        snprintf(si->first_unsupported, sizeof si->first_unsupported, "stage %d", stage);
        sm86_debug_dump("program", code, bytes, sph, si->first_unsupported);
        return NULL;
    }
    u32 n = bytes / 16;
    Ctx* c = calloc(1, sizeof *c);
    u64* words = malloc((size_t)n * 16);
    Sm86Insn* insn = malloc((size_t)n * sizeof *insn);
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
    if (stage == ST_TS || stage == ST_MS) mesh_layout(c);
    sm86_load(code, n, words);
    u32 top = 0;
    for (u32 i = 0; i < n; i++) {
        if (!sm86_decode(words[2 * i], words[2 * i + 1], &insn[i]))
            fail(c, "unknown opcode 0x%03x", (u32)(words[2 * i] & 0xfff));
        else if (!stage_ok(c, &insn[i]))
            fail(c, "%s in stage %d", sm86_ops[insn[i].op].name, stage);
        u32 r = max_register(&insn[i]);
        if (r > top) top = r;
    }
    /* The fixed function's varyings (colors, fog, texture coordinates) move to generics 17-31, as spirv_translate.c
       moves Maxwell's (shader_legacy_varyings), so that the stages of either ISA agree. A program that reads or
       writes a group and also its generic is refused */
    u32 legacy[2] = {0}, generic[2] = {0};
    for (u32 i = 0; i < n; i++) {
        const Sm86Insn* in = &insn[i];
        u32 addr, count, out;
        if (in->op == SM86_IPA && stage == ST_FS)
            addr = sm86_field(in, 64, 8) * 4, count = 1, out = 0;
        else if ((in->op == SM86_AST && stage == ST_VS) || (in->op == SM86_ALD && stage == ST_FS))
            addr = sm86_field(in, 40, 10), count = sm86_field(in, 74, 2) + 1, out = in->op == SM86_AST;
        else
            continue;
        for (u32 k = 0; k < count; k++) {
            int g = shader_legacy_group(addr + 4 * k);
            if (g >= 0)
                legacy[out] |= 1u << g;
            else if (addr + 4 * k >= 0x80 && addr + 4 * k < 0x280)
                generic[out] |= 1u << ((addr + 4 * k - 0x80) >> 4);
        }
    }
    for (u32 d = 0; d < 2 && !c->failed; d++)
        if (legacy[d] & generic[d] >> LEGACY_GENERIC)
            fail(c, "a legacy varying %s its generic, which the program also %s", d ? "written as" : "read as",
                 d ? "writes" : "reads");
    c->legacy_fs = stage == ST_FS ? legacy[0] : 0;
    /* the CALLs inlined: the program as the translation sees it */
    for (u32 i = 0; i < n && !c->failed; i++)
        if (insn[i].op == SM86_CALL) {
            Sm86Insn* flat = inline_calls(c, insn, &n);
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
        for (u32 r = 0; r <= top + 3 && r < SM86_RZ; r++)
            c->reg[r] = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        /* the local memory of LDL and STL: the SPH's bytes (words 1 and 2, bits 0-23, as Maxwell's), a power of two
           of words, or 4 KiB when the header gives none (compute), as spirv_translate.c makes Maxwell's */
        for (u32 i = 0; i < n && !c->local; i++)
            if (insn[i].op == SM86_LDL || insn[i].op == SM86_STL || insn[i].op == SM86_STL_NOU) {
                u32 bytes = sph ? (sph[1] & 0xffffffu) + (sph[2] & 0xffffffu) : 0;
                c->local_words = bytes ? 1 : 1024;
                while (bytes && c->local_words < 65536 && c->local_words * 4 < bytes) c->local_words <<= 1;
                c->local_ptr = spv_emit_type_function_pointer(e, c->t_u32);
                c->local = spv_emit_function_variable(
                    e, spv_emit_type_function_pointer(e, spv_emit_type_array_u32(e, c->t_u32, c->local_words)));
            }
        for (u32 r = 0; r < SM86_URZ; r++) c->ureg[r] = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        for (u32 p = 0; p < SM86_PT; p++) {
            c->pred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
            c->upred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
        }
        prepare_handles(c);
        /* a vertex program's outputs, made before its code: every exit's depth fix sees the position, and each
           starts as (0, 0, 0, 1), which the console gives the components a program doesn't write (spirv_translate.c
           does the same for Maxwell's). One game's full screen passes write only the position's x and y, and with
           an undefined w their triangles drew nothing */
        for (u32 i = 0; i < n && stage == ST_VS; i++)
            if (insn[i].op == SM86_AST)
                for (u32 k = 0; k <= sm86_field(&insn[i], 74, 2); k++)
                    output_var(c, sm86_field(&insn[i], 40, 10) + 4 * k);
        StructEmit se = {0};
        se.st = st;
        se.open = 1;
        se.label = calloc(4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1, sizeof *se.label + 1);
        if (!se.label) abort();
        se.used = (u8*)(se.label + 4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1);
        if (st->uses_flag) se.flag = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        if (stage == ST_VS) {
            u32 parts[4] = {cf(c, 0.0f), cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
            u32 empty = spv_emit_constant_vec4(e, c->t_v4f, parts);
            if (c->position) spv_emit_store(e, c->position, empty);
            for (u32 i = 0; i < 32; i++)
                if (c->out_attr[i]) spv_emit_store(e, c->out_attr[i], empty);
        }
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
                const Sm86Insn* in = &insn[i];
                int always = in->pred == SM86_PT && !in->pred_not, never = in->pred == SM86_PT && in->pred_not;
                if (never) continue;
                if (is_brx(in->op)) { /* the block's end: SHS_SWITCH takes the target's instruction */
                    u32 index = in->op == SM86_BRXU ? urd(c, sm86_field(in, 24, 6)) : rd(c, sm86_field(in, 24, 8));
                    c->branch_target = spv_emit_shift_right_u32(
                        e, c->t_u32, spv_emit_iadd_u32(e, c->t_u32, index, cu(c, brx_bias(in, i))), cu(c, 4));
                    continue;
                }
                if (in->op == SM86_BRA) { /* the block's end: SHS_IF takes its condition */
                    c->branch_cond = always ? 0 : prd(c, in->pred, in->pred_not, 0);
                    continue;
                }
                if (in->op == SM86_EXIT && always) {
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
                if (in->op == SM86_EXIT) {
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
        int mesh = stage == ST_TS || stage == ST_MS;
        if (!c->failed && mesh) {
            /* the program is the body of an entry point of its own */
            mesh_capability(c);
            fn = mesh_entry(c, fn);
            spv_emit_local_size(e, fn, si->mesh.threads, 1, 1);
            if (stage == ST_MS) {
                spv_emit_execution_mode(e, fn, 26 /* OutputVertices */, 1, si->mesh.max_vertices);
                spv_emit_execution_mode(e, fn, 5270 /* OutputPrimitivesEXT */, 1, si->mesh.max_primitives);
                spv_emit_execution_mode(e, fn, 5298 /* OutputTrianglesEXT */, 0, 0);
            }
        }
        if (!c->failed) {
            SpvShaderStage model = stage == ST_TS   ? SPV_STAGE_TASK
                                   : stage == ST_MS ? SPV_STAGE_MESH
                                                    : (SpvShaderStage)stage;
            spv_emit_entry_str(e, model, fn, "main", c->interfaces, c->ninterfaces);
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
    if (!out && si->unsupported) sm86_debug_dump("program", code, bytes, sph, si->first_unsupported);
    free(st);
    free(c->addr_key);
    free(c->handle);
    free(c->jump_first);
    free(insn);
    free(words);
    free(c);
    return out;
}
