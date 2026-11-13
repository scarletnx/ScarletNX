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
#include "nx/gpu/shader/write_log.h"
#include "nx/gpu/shader/minmax_spirv.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/bits.h"
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
    u32 gbuf_f32[16];  /* each global buffer's view as floats, for OpAtomicFAddEXT (atomic_fadd) */
    u32 cas_var;       /* atomic_fadd's loop's word, made at the start where the device has no float atomics */
    u32 wpages, mbase; /* SNX_VKB_WPAGES, and SNX_VKB_MBASE (the windows of pointer tables, SH_GBUF_TABLE) */
    u32 gkey;   /* the global buffer the last global_address named */
    u32 shared, shared_ptr, shared_words;
    u32 local, local_ptr, local_words; /* LDL and STL's memory: a Function array, made at the start */
    u32 in_attr[32], out_attr[32], position, point_size, fragcoord, color[8], depth;
    u32 vertex_index, instance_index, local_id, group_id, lane_id;
    u32 num_groups, hmma_count;  /* NumWorkgroups and the HMMAs so far, for hmma_zero */
    int coop;                    /* HMMA takes hmma_coop (coop_wanted), and the module the Vulkan memory model */
    u32 coop_slots, subgroup_id; /* hmma_coop's slots (Workgroup) and SubgroupId */
    u32 push, push_ptr; /* a vertex program's push constant vec4: the base instance, the depth fix (vk/render.c) */
    u32 centroid_mask;  /* the inputs decorated Centroid */
    u32 legacy_fs;      /* a fragment program's legacy varying groups (shader_legacy_group), read as generics 17-31 */
    u32 point_coord, front_facing;
    /* a fragment program at a resolution scale: SNX_VKB_RS's vec4 (scale_component) and the fragment stage's push
       constants, whose uvec4s mark the textures bound to scaled render targets (scaled_slot) */
    u32 scale, scale_ptr, scaled_push;
    u32 tex[SH_MAXTEX], tex_sampled[SH_MAXTEX];
    /* an indexed slot's arrays on SNX_VKB_TEXARRAY and SNX_VKB_TEXDEPTH, by kind (its tex[] is the element of the
       instruction being written), and SNX_VKB_TEXMAP and SNX_VKB_TEXFUNCS */
    u32 tex_array[SH_MAXTEX], tex_depth[SH_MAXTEX], tex_depth_type[SH_MAXTEX];
    u32 tex_map, tex_map_ptr, tex_func, tex_func_ptr;
    TexHandle* handle; /* by instruction, what prepare_handles found: NULL for a program without TEX.B */
    /* texture handles from memory (memory_handles): by instruction, whether it is one of ShaderInfo.memh, and its
       index there + 1; the values the instructions read, each from its writer (MhDep), for mh_finish */
    u8* mh_mark;
    u32* mh_pos;
    struct MhDep* mh_dep;
    u32 nmh, nmh_dep, mh_dep_cap;
    /* a handle's instructions being found (memory_handles): those marked since it began, to unmark when it can't be
       found, and why it can't (mh_fail) */
    u32* mh_new;
    u32 nmh_new;
    char mh_why[96];
    u32 img[8], img_type[8];
    u32 interfaces[MAX_INTERFACES], ninterfaces;
    /* the uniform register pair a ULDC.64 last loaded from a constant buffer: (bank << 16 | offset) + 1, 0 for none */
    /* by instruction: the global buffer (SH_GBUF_*) + 1 of an LDG's or STG's base, then (n + 1 on) the opcode that
       wrote its address, then (2 (n + 1) on) whether the address came from memory (track_step) */
    u32* addr_key;
    u32 dyn_keys[16], ndyn_keys; /* the keys an address may be one of where they met (track_conflict) */
    u32 table_banks;             /* the banks whose pointer tables (SH_GBUF_TABLE) addresses come from */
    u32 table_cells;             /* ShaderInfo.table_cells */
    u32 pairs[8][2], npairs; /* the two constant buffer bases of an address that adds both (track_step) */
    u32* jump_first; /* by instruction, the first of the BRAs and BRX that go to it in jump_from (n + 1 words), and */
    u32* jump_from;  /* those instructions: made the first time reg_const_at needs them (flow_sources) */
    u32 cur;         /* the instruction being written */
    u32 branch_cond; /* the condition of the BRA that ends the body before, 0 for always */
    u32 branch_target; /* the instruction that the BRX which ends the body before goes to */
    /* a task or mesh program's ISBE (isbe_var): a mesh program's attribute area and map (Workgroup), a task
       payload (TaskPayloadWorkgroupEXT), their words, and LocalInvocationIndex */
    u32 isbe_attr, isbe_map, payload, isbe_attr_words, isbe_map_words, payload_words, local_index;
    u32 mesh_first;                    /* the uniform buffer of an indirect mesh draw's first group (workgroup_index) */
    u32 vertex_words, primitive_words; /* the output words of a vertex and of a primitive (omap_words) */
    u32 prim_attr[32];                 /* a mesh program's per-primitive generic outputs */
    int soft_ftz;                      /* FTZ instructions flush their operands and results here (sm86_to_spirv_ftz) */
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
static u32 to_f(Ctx* c, u32 v) { return spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_f32, v); }
static u32 to_u(Ctx* c, u32 v) { return spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_u32, v); }
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
    return negate ? spv_emit_unary(&c->e, SPV_OP_LOGICAL_NOT, c->t_bool, v) : v;
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
        element = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                                  spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, at, cu(c, 4)),
                                  cu(c, elements - 1));
        component =
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                            spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, at, cu(c, 2)), cu(c, 3));
    } else {
        element = cu(c, (off >> 4) & (elements - 1));
        component = cu(c, (off >> 2) & 3);
    }
    return spv_emit_uniform_component(&c->e, c->cbuf_ptr, var, c->zero, element, component, c->t_u32);
}
/* NAK's MemType U8 (0), I8 (1), U16 (2), I16 (3) at byte offset off of a bank, plus index when it isn't 0: a byte or
   a halfword of the word that holds it, zero or sign extended, as mem_load takes one from memory. LDC's, ULDC's and
   ULDC.IDX's */
static u32 cbuf_subword(Ctx* c, u32 bank, u32 off, u32 index, u32 type) {
    u32 at = index ? spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off)) : cu(c, off);
    u32 shift =
        spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, at, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
    u32 word = cbuf_load(c, bank, off, index), bits = cu(c, type < 2 ? 8 : 16);
    return type & 1 ? to_u(c, spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                                               spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, word), shift, bits))
                    : spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, word, shift, bits);
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
    return s->neg && s->kind != SM86_SRC_IMM ? spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, c->zero, v) : v;
}
/* a float source with its absolute value and negation */
static u32 src_f(Ctx* c, const Sm86Src* s) {
    u32 v = to_f(c, src_bits(c, s));
    if (s->kind == SM86_SRC_IMM) return v;
    if (s->abs) v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, v);
    if (s->neg) v = spv_emit_unary(&c->e, SPV_OP_F_NEGATE, c->t_f32, v);
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
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, positive, low, cf(c, 0.0f));
}
/* where an instruction runs: its guard, and a BRA's or an EXIT's predicate operand (sm86_has_cond) as well. One game's
   interface programs exit the lanes their KILL took with @P0 EXIT P1: with P1 left out, every lane of a draw with P0
   set exited before the MOVs to its outputs, and the whole title screen drew transparent black */
static u32 guard(Ctx* c, const Sm86Insn* in) {
    u32 v = prd(c, in->pred, in->pred_not, 0), p = sm86_field(in, 87, 3), negate = sm86_field(in, 90, 1);
    if (!sm86_has_cond(in) || (p == SM86_PT && !negate)) return v;
    return op2(c, SPV_OP_LOGICAL_AND, c->t_bool, v, prd(c, p, negate, 0));
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
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, u64_type(c), p, c64(c, 1), c64(c, 0));
}
/* a source of a 64-bit sum: its two's complement as NAK's fold reads it, ~x + 1 in 33 bits */
static u32 term64(Ctx* c, const Sm86Src* s) {
    u32 v = src_bits(c, s);
    if (!s->neg || s->kind == SM86_SRC_IMM) return to64(c, v);
    return op2(c, SPV_OP_I_ADD, u64_type(c), to64(c, spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, v)), c64(c, 1));
}

/* ---- FTZ and the rounding modes ---- */

/* An FTZ instruction (bit 80) reads and writes a subnormal as the zero of its sign: per operand and result, as
   spirv_translate.c's float_ftz does in SH_FTZ_EMULATE, where the module's execution mode or the device does not do
   it (soft_ftz). Each such flush is four instructions: one vertex program of a game was 3.8 MB of SPIR-V with them and
   1.8 MB without, and the driver took 10 s to make its pipeline */
static u32 ftz(Ctx* c, u32 v, int on) {
    return on && c->soft_ftz ? spv_emit_flush_subnormal(&c->e, c->t_f32, c->t_u32, c->t_bool, c->glsl, v, 1) : v;
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
    u32 negative = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, cu(c, 0x7fffffffu), bits);
    u32 positive = spv_emit_unary(&c->e, SPV_OP_LOGICAL_NOT, c->t_bool, negative);
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
    u32 minus = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, bits, cu(c, 1));
    u32 is_neg_zero = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, bits, cu(c, 0x80000000u));
    u32 is_pos_zero = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, bits, c->zero);
    u32 up_bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, positive, plus,
                                   spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, is_neg_zero, cu(c, 1), minus));
    u32 down_bits =
        spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, negative, plus,
                         spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, is_pos_zero, cu(c, 0x80000001u), minus));
    bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, up, up_bits,
                            spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, down, down_bits, bits));
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
    if (mode == 0) return nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, x, y));
    return round_sum(c, mode, widen(c, x), widen(c, y));
}
/* FMUL: x * y * 2^(scale - 4) in mode. The product of two floats is exact in a double */
static u32 float_mul(Ctx* c, u32 mode, u32 scale, u32 x, u32 y) {
    if (mode == 0 && scale == 4) return nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, x, y));
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
    u32 sum =
        nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, spv_emit_vec4_extract(&c->e, c->t_f32, value, 2),
                                      spv_emit_vec4_extract(&c->e, c->t_f32, value, 3)));
    u32 depth = nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, sum, cf(c, 0.5f)));
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
    u32 odd = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, bits, cu(c, 16)), cu(c, 1));
    u32 rounded = spv_emit_binary(
        &c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
        spv_emit_iadd_u32(&c->e, c->t_u32, bits, spv_emit_iadd_u32(&c->e, c->t_u32, cu(c, 0x7fff), odd)), cu(c, 16));
    u32 nan = spv_emit_unary(&c->e, SPV_OP_IS_NAN, c->t_bool, f);
    return spv_emit_ternary(
        &c->e, SPV_OP_SELECT, c->t_u32, nan,
        spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                        spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, bits, cu(c, 16)), cu(c, 0x40)),
        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, rounded, cu(c, 0xffff)));
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
/* ---- a fragment program at a resolution scale (resolution= other than 1) ---- */

/* The render targets' images are the resolution scale times the guest's size, and a fragment program counts the
   guest's pixels and texels, as the Maxwell translator gives them to its programs (spirv_translate.c's needs_scale
   and shader_scaled_fetch): the fragment's x and y are FragCoord's over the scale, a fetch from a texture bound to
   a scaled target reads texel p in the pixels the image has for it, and a size query of one answers its size over
   the scale. Other textures (one a compute program stored, one from memory) keep their own texels. Left in the
   target's pixels, a fetch at the fragment's own coordinates read a texture of the guest's size over the target's
   top-left corner alone, and past the texture's edge everywhere else */
/* component k of SNX_VKB_RS's vec4, which the renderer gives the fragment stage (vk/render.c's frame_consts): 0 and
   1 are 1 / the resolution scale, 2 the scale */
static u32 scale_component(Ctx* c, u32 k) {
    if (!c->scale) {
        c->scale = spv_emit_uniform_vec4_array(&c->e, c->t_v4f, c->t_f32, c->t_u32, (u32)shader_vk_set(ST_FS),
                                               SNX_VKB_RS, 1, &c->scale_ptr);
        add_interface(c, c->scale);
    }
    return spv_emit_uniform_component(&c->e, c->scale_ptr, c->scale, c->zero, c->zero, cu(c, k), c->t_f32);
}
/* Whether texture slot is bound to a scaled render target: its bit in the uvec4 after the fragment stage's vec4 at
   push constant offset 16 (vk/render.c: x the slots 0 to 31, y 32 to 63), as a bool. With own, whether that target
   has the size of the one the draw renders to as well (render.c's own_size): its bit in the uvec4 at offset 96 */
static u32 scaled_slot(Ctx* c, int slot, int own) {
    if (!c->scaled_push) {
        const u32 members[3] = {c->t_v4f, c->t_v4u, c->t_v4u}, offsets[3] = {16, 32, 96};
        u32 block = spv_emit_type_struct(&c->e, members, offsets, 3);
        spv_emit_decorate(&c->e, block, 2, NULL, 0);         /* Block */
        c->scaled_push = spv_emit_variable(&c->e, 9, block); /* 9: PushConstant */
        add_interface(c, c->scaled_push);
    }
    u32 bits = spv_emit_push_component_f32(&c->e, spv_emit_type_pointer(&c->e, 9, c->t_u32), c->scaled_push,
                                           cu(c, own ? 2 : 1), cu(c, slot >= 32), c->t_u32);
    u32 bit = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, bits, cu(c, (u32)slot & 31)),
                              cu(c, 1));
    return spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool, bit, c->zero);
}
static u32 fl(Ctx* c, u32 v) { return spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_FLOOR, v); }
/* FragCoord's component k, in the pixels of the target's image */
static u32 frag_pixel(Ctx* c, u32 k) {
    if (!c->fragcoord) {
        c->fragcoord = spv_emit_fragcoord_input(&c->e, c->t_v4f);
        add_interface(c, c->fragcoord);
    }
    return spv_emit_vec4_extract(&c->e, c->t_f32, spv_emit_load(&c->e, c->t_v4f, c->fragcoord), k);
}
/* A fragment program's 2D texel coordinates *x and *y (bits of signed integers) of texture slot, as the image has
   them. The image of a scaled target has texel p from pixel p times the scale on. Where the target has the size of
   the one drawn to, the pixel that lies where the fragment lies in its own guest pixel is fetched, as Eden's
   rescaling pass places it (SubScale): at a whole scale a pass that fetches its own pixel reads that very pixel, not
   one for all the fragments of a guest pixel. It stays within the texel's pixels, which at another scale end before a
   whole one. A target of another size (a table of a few texels, a smaller level) has no such pixel for the fragment,
   and its place there would give a texel's value by the fragment's column and row: the pixel at the texel's centre
   is fetched, as the Maxwell translator fetches it (shader_scaled_fetch). Another texture keeps p */
static void scaled_fetch(Ctx* c, int slot, u32* x, u32* y) {
    u32 scaled = scaled_slot(c, slot, 0), own = scaled_slot(c, slot, 1);
    u32 scale = scale_component(c, 2), over = scale_component(c, 0);
    u32* v[2] = {x, y};
    for (u32 k = 0; k < 2; k++) {
        u32 p = spv_emit_unary(&c->e, SPV_OP_CONVERT_S_TO_F, c->t_f32,
                               spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, *v[k]));
        u32 first = nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, p, scale));
        u32 next = fl(c, nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, first, scale)));
        u32 last = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_MAX, fl(c, first),
                                        nocontract(c, op2(c, SPV_OP_F_SUB, c->t_f32, next, cf(c, 1.0f))));
        /* the fragment's place in its guest pixel, in the image's pixels: 0 up to the scale */
        u32 pixel = frag_pixel(c, k);
        u32 corner = nocontract(
            c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, scale,
                               fl(c, nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, pixel, over)))));
        u32 place = nocontract(c, op2(c, SPV_OP_F_SUB, c->t_f32, pixel, corner));
        u32 at = spv_emit_glsl_binary(
            &c->e, c->t_f32, c->glsl, SPV_GLSL_F_MIN,
            fl(c, nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, first, place))), last);
        u32 centre = fl(
            c,
            nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32,
                                          nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, p, cf(c, 0.5f))),
                                          scale)));
        at = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, own, at, centre);
        *v[k] = spv_emit_ternary(
            &c->e, SPV_OP_SELECT, c->t_u32, scaled,
            spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_u32, spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32, at)),
            *v[k]);
    }
}
/* The size *x by *y of texture slot's image as the guest has it: a scaled target's over the scale, rounded as the
   renderer rounds a scaled size */
static void scaled_size(Ctx* c, int slot, u32* x, u32* y) {
    u32 factor =
        spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, scaled_slot(c, slot, 0), scale_component(c, 0), cf(c, 1.0f));
    u32* v[2] = {x, y};
    for (u32 k = 0; k < 2; k++) {
        u32 f = nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32,
                                              spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32, *v[k]), factor));
        *v[k] = spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_U, c->t_u32,
                               nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, f, cf(c, 0.5f))));
    }
}
/* FragCoord's component k: x and y in the guest's pixels, z and w as they are */
static u32 frag_coord(Ctx* c, u32 k) {
    u32 v = frag_pixel(c, k);
    return k < 2 ? nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, v, scale_component(c, 0))) : v;
}
/* ALD's attribute at byte address addr, as bits */
static u32 attr_load(Ctx* c, u32 addr) {
    if (addr >= 0x80 && addr < 0x280) return input_load(c, (addr - 0x80) >> 4, (addr >> 2) & 3);
    if (c->stage == ST_VS && addr == 0x2f8) /* InstanceIndex counts from the base instance, the push constant's x */
        return spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, builtin_u32(c, &c->instance_index, 43),
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
           never finished: the GPU's context timed out. The Vulkan memory model (c->coop) has no Coherent: there the
           loads and stores are relaxed atomics instead (mem_word_load), which Mesa makes the same coherent accesses */
        spv_emit_decorate(&c->e, c->gbuf[slot], 20 /* Aliased */, NULL, 0);
        if (!c->coop) spv_emit_decorate(&c->e, c->gbuf[slot], 23 /* Coherent */, NULL, 0);
        add_interface(c, c->gbuf[slot]);
    }
    if (write)
        c->si->gbuf_wmask |= 1u << slot;
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
   the write log (ShaderInfo.gbuf_mark, write_log.c's shader_wlog_store). The renderer lands the bytes logged and no
   others: an Ounce program's buffer is all of the memory mapped from its address, up to 16 MiB, and finding what
   changed by comparing its bytes with a copy taken before the dispatch was most of one game's CPU time */
static void mark_written(Ctx* c, u32 key, u32 offset, u32 bytes) {
    int slot = -1;
    if (c->stage != ST_CS) return;
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) slot = i;
    if (slot < 0) return;
    u32 part = shader_wlog_part(c->si, slot);
    if (!c->wpages) {
        c->wpages = shader_wlog_buffer(&c->e, c->t_u32, (u32)shader_vk_set(c->stage));
        add_interface(c, c->wpages);
    }
    shader_wlog_store(&c->e, c->t_u32, c->t_bool, c->wpages, part, offset, bytes);
}
/* the base address of global buffer key (SH_GBUF_*) as the program finds it when it runs: the constant buffer word,
   or an indirect buffer's pointer at its cell in its parent buffer, which the program loaded itself (the renderer binds
   the memory from the pointer it reads there). The low word, or with wide the 64-bit address. 0 when it can't */
static u32 mem_ptr(Ctx* c, u32 buffer, u32 address, u32 word);
static u32 mem_word_load(Ctx* c, u32 buffer, u32 ptr);
static u32 gbuf_base(Ctx* c, u32 key, int wide) {
    u32 parent = SH_GBUF_PARENT(key);
    if (SH_GBUF_BANK(parent) >= 18) { /* the renderer reads the base from a bound bank (vk/render.c) */
        fail(c, "global address whose base is in constant buffer %u", SH_GBUF_BANK(parent));
        return 0;
    }
    if (SH_GBUF_TABLE(key)) { /* the window's base, at its buffer's slot of SNX_VKB_MBASE */
        int slot = -1;
        for (int i = 0; i < c->si->ngbuf; i++)
            if (c->si->gbuf_cbuf_off[i] == key) slot = i;
        if (slot < 0) return 0;
        if (!c->mbase) {
            c->mbase = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                                   SNX_VKB_MBASE, 16, &c->cbuf_ptr);
            add_interface(c, c->mbase);
        }
        u32 low =
            spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mbase, c->zero, cu(c, (u32)slot), c->zero, c->t_u32);
        return wide ? pair64(c, low,
                             spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mbase, c->zero, cu(c, (u32)slot),
                                                        cu(c, 1), c->t_u32))
                    : low;
    }
    if (SH_GBUF_INDIRECT(key)) {
        u32 buffer = gbuf_slot(c, parent, 0), cell = cu(c, SH_GBUF_CELL(key));
        if (!buffer) return 0;
        u32 low = mem_word_load(c, buffer, mem_ptr(c, buffer, cell, 0));
        return wide ? pair64(c, low, mem_word_load(c, buffer, mem_ptr(c, buffer, cell, 1))) : low;
    }
    if (wide)
        return pair64(c, cbuf_load(c, SH_GBUF_BANK(key), SH_GBUF_OFFSET(key), 0),
                      cbuf_load(c, SH_GBUF_BANK(key), SH_GBUF_OFFSET(key) + 4, 0));
    return cbuf_load(c, SH_GBUF_BANK(key), SH_GBUF_OFFSET(key), 0);
}
/* LDG and STG, as spirv_translate.c's global memory: the buffer is the one whose 64-bit address the constant buffer
   word that track_globals found holds (ShaderInfo.gbuf_cbuf_off), or an indirect buffer's pointer (gbuf_base), and the
   byte offset into it is the address less that base, read at run time. The address is a register (a pair with bit 90
   for the uniform form, bit 72 for the other) and a uniform register pair (bit 72), with a signed 24-bit offset; the
   low 32 bits of the difference serve */
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
    u32 base = gbuf_base(c, key, 0);
    if (!base) return 0;
    *offset = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, at, base);
    c->gkey = key;
    return gbuf_slot(c, key, write);
}
/* An LDG, STG, ATOMG or RED whose address is a pointer that the program loaded from memory, which no constant buffer
   word holds, as one game's vertex program that clears the indirect draws a table of pointers names. As the Maxwell
   translator does (shader_global_dynamic, after Eden's DefineGlobalMemoryFunctions), its 64-bit address is compared
   with each global buffer of the program when it runs, and the first whose bound bytes hold it takes the access: the
   renderer binds the memory mapped from the address its word holds, at most 16 MiB (vk_gbuf_desc), and its length is
   the buffer's (OpArrayLength), so an address past a shorter one goes on to the next. A load from none of them reads 0
   and a store there is dropped, where the console's GPU reaches all of its address space; an atomic does its
   operation in the buffer that holds the address and returns what was there, 0 from none. The address: a register,
   a pair with bit 90 in the uniform form of LDG and STG or bit 72 in the others, plus the uniform register (a pair
   with bit 72) of the uniform forms (LDG's at 32, STG's and ATOMG's at 64), plus the signed 24-bit offset. RED adds
   the uniform register at 64 only with bit 91: one game's mesh programs' RED of [UR4] have it, and its culling
   programs' RED of a register pair have bits 64-71 0 without it, where an unused uniform register would be URZ (63).
   Its buffers: those of the program's accesses, the indirect ones an address may be one of (track_conflict): one
   game's compute programs pick one of two pointers they load with a predicate, then index the table it names, and the
   windows of a pointer table (SH_GBUF_TABLE): its culling programs count and fill their lists at pointers they load
   from cbuf 4 */
enum { DYN_STORE, DYN_LOAD, DYN_ATOMIC };
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst);
static void mem_store(Ctx* c, u32 buffer, u32 address, u32 type, u32 data);
static u32 atomic_fadd(Ctx* c, u32 buffer, u32 address, u32 data);
static void global_dynamic(Ctx* c, const Sm86Insn* in, int kind, u32 opcode, int fadd) {
    int op = in->op, ldst_uniform = op == SM86_LDG || op == SM86_STG;
    u32 type = sm86_field(in, 73, 3), words = kind == DYN_ATOMIC ? 1 : type == 5 ? 2 : type == 6 ? 4 : 1;
    u32 reg = sm86_field(in, 24, 8), imm = sm86_field(in, 40, 24);
    u32 hi = sm86_field(in, ldst_uniform ? 90 : 72, 1) && reg < SM86_RZ ? rd(c, reg + 1) : c->zero;
    u32 address = pair64(c, rd(c, reg), hi);
    u64 bias = imm & 0x800000u ? (u64)imm | 0xffffffffff000000ull : imm;
    address = op2(c, SPV_OP_I_ADD, u64_type(c), address, c64(c, bias));
    if (ldst_uniform || op == SM86_ATOMG || (op == SM86_RED && sm86_field(in, 91, 1))) {
        u32 ur = sm86_field(in, op == SM86_LDG ? 32 : 64, 6);
        if (ur < SM86_URZ) {
            u32 uhi = sm86_field(in, 72, 1) && ur + 1 < SM86_URZ ? urd(c, ur + 1) : c->zero;
            address = op2(c, SPV_OP_I_ADD, u64_type(c), address, pair64(c, urd(c, ur), uhi));
        }
    }
    int returns = kind == DYN_LOAD || (kind == DYN_ATOMIC && op != SM86_RED);
    /* an atomic's data before its destination is cleared: they may be one register */
    u32 data = kind == DYN_ATOMIC ? rd(c, sm86_field(in, 32, 8)) : 0;
    if (returns)
        for (u32 k = 0; k < words && in->dst + k < SM86_RZ; k++) wr(c, in->dst + k, c->zero);
    /* the program's buffers, each key once, in the order of their first access, then the candidates of addresses
       that met */
    u32 keys[16], nkeys = 0, pair = c->addr_key[3 * (c->n + 1) + c->cur];
    for (u32 k = 0; pair && k < 2; k++)
        if (SH_GBUF_BANK(SH_GBUF_PARENT(c->pairs[pair - 1][k] - 1)) < 18) keys[nkeys++] = c->pairs[pair - 1][k] - 1;
    for (u32 i = 0; !pair && i < c->n + c->ndyn_keys; i++) {
        u32 key = i < c->n ? c->addr_key[i] : c->dyn_keys[i - c->n] + 1, seen = 0;
        if (!key || SH_GBUF_BANK(SH_GBUF_PARENT(key - 1)) >= 18) continue;
        for (u32 k = 0; k < nkeys; k++) seen |= keys[k] == key - 1;
        if (!seen && nkeys < 16) keys[nkeys++] = key - 1;
    }
    u32 merges[16];
    for (u32 k = 0; k < nkeys; k++) {
        u32 buffer = gbuf_slot(c, keys[k], kind != DYN_LOAD);
        if (!buffer) return;
        if (kind == DYN_ATOMIC) mark_read(c, keys[k]);
        u32 base = gbuf_base(c, keys[k], 1);
        if (!base) return;
        u32 relative = op2(c, SPV_OP_I_SUB, u64_type(c), address, base);
        u32 length = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                     SPV(&c->e, 68 /* OpArrayLength */, c->t_u32, buffer, 0), cu(c, 2));
        u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, relative, to64(c, length));
        u32 then = spv_emit_id(&c->e), other = spv_emit_id(&c->e);
        merges[k] = spv_emit_id(&c->e);
        spv_emit_selection_merge(&c->e, merges[k]);
        spv_emit_branch_conditional(&c->e, inside, then, other);
        spv_emit_label(&c->e, then);
        u32 offset = lo32(c, relative);
        if (kind == DYN_LOAD)
            mem_load(c, buffer, offset, type, in->dst, 0);
        else if (kind == DYN_STORE) {
            mark_written(c, keys[k], offset, type < 2 ? 1 : type < 4 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 16);
            mem_store(c, buffer, offset, type, sm86_field(in, 32, 8));
        } else {
            mark_written(c, keys[k], offset, 4);
            u32 old = fadd ? atomic_fadd(c, buffer, offset, data)
                           : spv_emit_atomic_u32(&c->e, c->t_u32, opcode, mem_ptr(c, buffer, offset, 0), data);
            if (returns && in->dst < SM86_RZ) wr(c, in->dst, old);
        }
        spv_emit_branch(&c->e, merges[k]);
        spv_emit_label(&c->e, other);
    }
    for (u32 k = nkeys; k-- > 0;) {
        spv_emit_branch(&c->e, merges[k]);
        spv_emit_label(&c->e, merges[k]);
    }
}
/* what track_globals follows at an instruction: each register's key (a global buffer's SH_GBUF_* + 1, 0 for none),
   whether it holds that buffer's base itself with nothing added (where the program loads a pointer from, an
   indirect buffer's cell), and whether its value came from memory (a pointer the program loaded, through moves and
   arithmetic: global_dynamic's), and the bank + 1 of the pointer table it came from (SH_GBUF_TABLE), whether it holds
   one of the table's words itself (moved, nothing added), and the cells of the memory a table's word points to that
   it was loaded from (bit k: the 8 bytes at 8 * k, through arithmetic, ShaderInfo.table_cells), then the opcode that
   last wrote it (for the message of an address without a key), and whether it holds the constant 1 (a MOV of it). The
   keys and flags first: the flow settles on them */
typedef struct {
    u32 rkey[256], ukey[64];
    u8 rexact[256], uexact[64];
    u8 rdyn[256], udyn[64];
    u8 rtab[256], utab[64];
    u8 rtabx[256], utabx[64];
    u32 rcell[256], ucell[64];
    u8 rone[256], uone[64];
    u8 rpair[256], upair[64];
    u16 rsrc[256], usrc[64];
} TrackState;
#define TRACK_KEYS offsetof(TrackState, rsrc)
static int key_indirect(u32 key) { return key && SH_GBUF_INDIRECT(key - 1); }
/* where two keys meet (a predicated write over the register's key, or two paths): an indirect key against another
   key takes neither, and the address is then compared with each at run time (global_dynamic), which the two are
   candidates of. Two constant buffer keys keep track_globals's own rules */
static void track_conflict(Ctx* c, u32 a, u32 b) {
    for (u32 k = 0; k < 2; k++) {
        u32 key = k ? b : a, seen = 0;
        if (!key) continue;
        for (u32 j = 0; j < c->ndyn_keys; j++) seen |= c->dyn_keys[j] == key - 1;
        if (!seen && c->ndyn_keys < 16) c->dyn_keys[c->ndyn_keys++] = key - 1;
    }
}
/* the key of a register source of a global access or an ALU instruction (0 for none) */
static u32 src_key(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rkey[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->ukey[x->reg]
           : x->kind == SM86_SRC_CBUF && !x->bindless      ? (u32)(x->bank << 16 | x->value) + 1
                                                           : 0;
}
static int src_dyn(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rdyn[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->udyn[x->reg]
                                                           : 0;
}
static u32 src_tab(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rtab[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->utab[x->reg]
                                                           : 0;
}
static u32 src_pair(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rpair[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->upair[x->reg]
                                                           : 0;
}
static int src_one(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_IMM                         ? x->value == 1
           : x->kind == SM86_SRC_REG && x->reg < SM86_RZ   ? s->rone[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->uone[x->reg]
                                                           : 0;
}
static u32 src_cell(const TrackState* s, const Sm86Src* x) {
    return x->kind == SM86_SRC_REG && x->reg < SM86_RZ     ? s->rcell[x->reg]
           : x->kind == SM86_SRC_UREG && x->reg < SM86_URZ ? s->ucell[x->reg]
                                                           : 0;
}
/* a register (uniform when u) takes a key: a guarded write over an indirect key, or of one over another key, leaves it
   as either, a candidate pair for global_dynamic */
static void track_write(Ctx* c, TrackState* s, int u, u32 d, u32 key, int exact, int dyn, u16 op, int always) {
    u32* keys = u ? s->ukey : s->rkey;
    if (d >= (u ? SM86_URZ : SM86_RZ)) return;
    if (!always && keys[d] && key && keys[d] != key && (key_indirect(keys[d]) || key_indirect(key))) {
        track_conflict(c, keys[d], key);
        key = 0, exact = 0, dyn = 1;
    }
    keys[d] = key;
    (u ? s->uone : s->rone)[d] = 0;
    (u ? s->upair : s->rpair)[d] = 0;
    (u ? s->uexact : s->rexact)[d] = (u8)exact;
    (u ? s->udyn : s->rdyn)[d] = (u8)dyn;
    (u ? s->usrc : s->rsrc)[d] = op;
}
/* one instruction of track_globals: an LDG's or STG's key into addr_key, and what its destination gets */
static void track_step(Ctx* c, u32 i, TrackState* s) {
    const Sm86Insn* in = &c->in[i];
    int op = in->op, uniform = sm86_ops[op].kind == SM86_K_UALU, always = sm86_always(in);
    u32 n1 = c->n + 1;
    if (op == SM86_LDG || op == SM86_STG || op == SM86_LDG_NOU || op == SM86_STG_NOU || op == SM86_ATOMG ||
        op == SM86_ATOMG_NOU || op == SM86_RED) {
        /* RED adds its uniform register only with bit 91 (global_dynamic) */
        int uniform_form =
            op == SM86_LDG || op == SM86_STG || op == SM86_ATOMG || (op == SM86_RED && sm86_field(in, 91, 1));
        u32 reg = sm86_field(in, 24, 8), ur = uniform_form ? sm86_field(in, op == SM86_LDG ? 32 : 64, 6) : SM86_URZ;
        u32 k = ur < SM86_URZ && s->ukey[ur] ? s->ukey[ur] : reg < SM86_RZ ? s->rkey[reg] : 0;
        c->addr_key[i] = k;
        c->addr_key[n1 + i] = ur < SM86_URZ && s->ukey[ur] ? s->usrc[ur] : reg < SM86_RZ ? s->rsrc[reg] : 0;
        if (!k && ur < SM86_URZ && !s->ukey[ur] && s->usrc[ur]) c->addr_key[n1 + i] = s->usrc[ur];
        c->addr_key[2 * n1 + i] = (ur < SM86_URZ && s->udyn[ur]) || (reg < SM86_RZ && s->rdyn[reg]);
        c->addr_key[3 * n1 + i] = ur < SM86_URZ && s->upair[ur] ? s->upair[ur] : reg < SM86_RZ ? s->rpair[reg] : 0;
        u32 tab = ur < SM86_URZ && s->utab[ur] ? s->utab[ur] : reg < SM86_RZ ? s->rtab[reg] : 0;
        if (tab) c->table_banks |= 1u << (tab - 1);
        /* an address loaded from the memory a table's word points to: the renderer binds where those cells point
           too (ShaderInfo.table_cells) */
        c->table_cells |= (ur < SM86_URZ ? s->ucell[ur] : 0) | (reg < SM86_RZ ? s->rcell[reg] : 0);
        /* the address is one of the table's words itself, plus the offset: the pairs an LDG.64 or .128 loads there
           are its target's cells */
        int table_word = ur < SM86_URZ ? reg >= SM86_RZ && s->utabx[ur] : reg < SM86_RZ && s->rtabx[reg];
        if (op == SM86_LDG || op == SM86_LDG_NOU || op == SM86_ATOMG || op == SM86_ATOMG_NOU) {
            /* the words loaded are data: one, or two or four for an LDG.64 or .128, from memory */
            u32 type = sm86_field(in, 73, 3), words = op == SM86_ATOMG || op == SM86_ATOMG_NOU ? 1
                                                      : type == 5                              ? 2
                                                      : type == 6                              ? 4
                                                                                               : 1;
            /* An LDG.64 or .128 of a constant buffer key's base itself plus the offset: each pair it loads is the
               address of an indirect buffer (SH_GBUF_INDIRECT) whose cell is there. Only the uniform register's or
               only the register's base, exact, and a cell the key can name */
            u32 cell = 0, imm = sm86_field(in, 40, 24);
            int exact =
                ur < SM86_URZ ? s->ukey[ur] == k && s->uexact[ur] && reg >= SM86_RZ : reg < SM86_RZ && s->rexact[reg];
            if (words >= 2 && (op == SM86_LDG || op == SM86_LDG_NOU) && k && !key_indirect(k) && exact &&
                SH_GBUF_PARENT(k - 1) == k - 1 && !(imm & 7) && imm + 8 * (words / 2 - 1) <= SH_GBUF_CELL_MAX)
                cell = (k - 1) | 1u << 31 | imm / 8 << 21;
            int cells = table_word && words >= 2 && (op == SM86_LDG || op == SM86_LDG_NOU) && !(imm & 7) &&
                        imm + 8 * (words / 2 - 1) < 8 * 32;
            for (u32 w = 0; w < words && in->dst + w < SM86_RZ; w++) {
                u32 key = cell && !(w & 1) ? (cell + (w / 2 << 21)) + 1 : 0;
                track_write(c, s, 0, in->dst + w, key, key != 0, !key, (u16)op, always);
                s->rtab[in->dst + w] = cells ? (u8)tab : 0;
                s->rtabx[in->dst + w] = 0;
                s->rcell[in->dst + w] = cells ? 1u << (imm + 4 * (w & ~1u)) / 8 : 0;
            }
        }
        return;
    }
    if (op == SM86_LDS || op == SM86_LDL) { /* data from memory, as a load from global memory's */
        u32 type = sm86_field(in, 73, 3), words = type == 5 ? 2 : type == 6 ? 4 : 1;
        for (u32 w = 0; w < words && in->dst + w < SM86_RZ; w++) {
            track_write(c, s, 0, in->dst + w, 0, 0, 1, (u16)op, always);
            s->rtab[in->dst + w] = 0, s->rtabx[in->dst + w] = 0, s->rcell[in->dst + w] = 0;
        }
        return;
    }
    u32 key = 0, tab = 0, tabx = 0, cellm = 0, pair = 0;
    int found = 0, exact = 0, dyn = 0;
    if (op == SM86_ULDC || (op == SM86_LDC && sm86_field(in, 24, 8) == SM86_RZ))
        key = (sm86_field(in, 54, 5) << 16 | sm86_field(in, 38, 16)) + 1, found = 1, exact = 1;
    else if (op == SM86_MOV || op == SM86_UMOV || op == SM86_R2UR) {
        const Sm86Src* b = &in->b;
        if (op == SM86_R2UR) {
            u32 r = sm86_field(in, 24, 8) & 255;
            if (r < SM86_RZ)
                key = s->rkey[r], exact = s->rexact[r], dyn = s->rdyn[r], tab = s->rtab[r], tabx = s->rtabx[r],
                cellm = s->rcell[r], pair = s->rpair[r];
        } else if (b->kind == SM86_SRC_CBUF && !b->bindless)
            key = (u32)(b->bank << 16 | b->value) + 1, exact = 1;
        else if (b->kind == SM86_SRC_REG && b->reg < SM86_RZ)
            key = s->rkey[b->reg], exact = s->rexact[b->reg], dyn = s->rdyn[b->reg], tab = s->rtab[b->reg],
            tabx = s->rtabx[b->reg], cellm = s->rcell[b->reg], pair = s->rpair[b->reg];
        else if (b->kind == SM86_SRC_UREG && b->reg < SM86_URZ)
            key = s->ukey[b->reg], exact = s->uexact[b->reg], dyn = s->udyn[b->reg], tab = s->utab[b->reg],
            tabx = s->utabx[b->reg], cellm = s->ucell[b->reg], pair = s->upair[b->reg];
        found = 1;
    } else if ((op == SM86_IADD3 || op == SM86_UIADD3 || op == SM86_IMAD || op == SM86_UIMAD || op == SM86_IMAD_WIDE ||
                op == SM86_UIMAD_WIDE || op == SM86_LEA || op == SM86_ULEA) &&
               !sm86_field(in, 74, 1) && !((op == SM86_LEA || op == SM86_ULEA) && sm86_field(in, 80, 1))) {
        /* one source with a base: the result is that base at an offset. A constant buffer operand is a base
           itself, as in ptxas's LEA R0, P0, R2, c[0x0][0x60], 0x2. A constant buffer key comes before an indirect
           one, which a pointer the program loaded has: one of each is the constant buffer's, as before there were
           indirect keys */
        const Sm86Src* s3[3] = {&in->a, &in->b, &in->c};
        u32 count = 0, first = 0, last = 3, indirect = 0, nindirect = 0, bases[3];
        int imad = op == SM86_IMAD || op == SM86_UIMAD || op == SM86_IMAD_WIDE || op == SM86_UIMAD_WIDE;
        if (imad)
            first = 2; /* a product is an offset: only the sum's c can be the base */
        else if (op == SM86_LEA || op == SM86_ULEA)
            first = 1, last = 2; /* the shifted a is an offset: only b */
        for (u32 k = first; k < last; k++) {
            u32 v = src_key(s, s3[k]);
            if (key_indirect(v))
                indirect = v, nindirect++;
            else if (v)
                key = v, bases[count++] = v;
        }
        /* but a product by 1 is its other factor, which may be the base: one game's program adds the address that
           c[0][0x20] holds to the offset in c[1][0] with UIMAD.WIDE UR4, UR6, UR8, UR4, UR6 1 */
        if (imad && (src_one(s, &in->a) || src_one(s, &in->b))) {
            u32 v = src_key(s, src_one(s, &in->a) ? &in->b : &in->a);
            if (v && !key_indirect(v)) key = v, bases[count++] = v;
        }
        if (count >= 2) {
            /* two bases, one of them an offset: the accesses at the sum compare their address with each of the two as
               the program runs (global_dynamic), and with no other buffer */
            for (u32 k = 0; k < c->npairs && !pair; k++)
                if (c->pairs[k][0] == bases[0] && c->pairs[k][1] == bases[1]) pair = k + 1;
            if (!pair && c->npairs < 8) {
                c->pairs[c->npairs][0] = bases[0], c->pairs[c->npairs][1] = bases[1];
                pair = ++c->npairs;
            }
            dyn = 1;
        }
        if (count != 1) key = !count && nindirect == 1 ? indirect : 0;
        found = 1;
    }
    /* a value from memory stays one through the arithmetic that moves an address: each source the instruction reads */
    switch (op) {
        case SM86_IADD3:
        case SM86_UIADD3:
        case SM86_IMAD:
        case SM86_UIMAD:
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE:
        case SM86_LOP3:
        case SM86_ULOP3:
            dyn |= src_dyn(s, &in->c);
            if (!tab) tab = src_tab(s, &in->c);
            cellm |= src_cell(s, &in->c);
            if (!pair) pair = src_pair(s, &in->c);
            /* fall through */
        case SM86_LEA:
        case SM86_ULEA:
        case SM86_SEL:
        case SM86_USEL:
            dyn |= src_dyn(s, &in->a) || src_dyn(s, &in->b);
            if (!tab) tab = src_tab(s, &in->a) ? src_tab(s, &in->a) : src_tab(s, &in->b);
            cellm |= src_cell(s, &in->a) | src_cell(s, &in->b);
            if (!pair) pair = src_pair(s, &in->a) ? src_pair(s, &in->a) : src_pair(s, &in->b);
            break;
        case SM86_SHF:
        case SM86_USHF:
            dyn |= src_dyn(s, &in->a) || src_dyn(s, &in->c);
            if (!tab) tab = src_tab(s, &in->a) ? src_tab(s, &in->a) : src_tab(s, &in->c);
            cellm |= src_cell(s, &in->a) | src_cell(s, &in->c);
            break;
        default: break;
    }
    /* a compute program's LDC.64 at an offset it computes: a pointer from the bank's table (SH_GBUF_TABLE), whose
       windows the addresses made from it are compared with (track_globals adds them once the flow settles) */
    if (op == SM86_LDC && sm86_field(in, 24, 8) < SM86_RZ && sm86_field(in, 73, 3) == 5 && c->stage == ST_CS &&
        sm86_field(in, 54, 5) < 18)
        dyn = 1, tab = sm86_field(in, 54, 5) + 1, tabx = 1;
    /* the destination: the key, or none for any other write (the high word of a pair takes none either) */
    u32 d = uniform || op == SM86_ULDC || op == SM86_UMOV || op == SM86_R2UR ? sm86_field(in, 16, 6) : in->dst;
    int to_uniform = uniform || op == SM86_ULDC || op == SM86_R2UR || op == SM86_S2UR || op == SM86_ULDC_IDX;
    /* a 64-bit result's high word takes no key: ULDC's and LDC's of type 5 alone. A ULDC of one word took the key of
       the register after its own, and one game's material program lost UR7, c[0][0x530] + 0x30, to a ULDC UR6 of a
       word a thousand instructions before its LDG */
    int wide = ((op == SM86_ULDC || op == SM86_LDC) && sm86_field(in, 73, 3) == 5) || op == SM86_IMAD_WIDE ||
               op == SM86_UIMAD_WIDE;
    if (!found) key = 0, exact = 0;
    int one = (op == SM86_MOV || op == SM86_UMOV) && src_one(s, &in->b);
    if (to_uniform) {
        track_write(c, s, 1, d, key, exact, dyn, (u16)op, always);
        if (d < SM86_URZ)
            s->utab[d] = (u8)tab, s->utabx[d] = (u8)tabx, s->ucell[d] = cellm, s->uone[d] = (u8)one,
                      s->upair[d] = (u8)pair;
        if (wide && d + 1 < SM86_URZ)
            s->ukey[d + 1] = 0, s->uexact[d + 1] = 0, s->udyn[d + 1] = (u8)dyn, s->utab[d + 1] = (u8)tab,
                      s->utabx[d + 1] = 0, s->ucell[d + 1] = cellm, s->uone[d + 1] = 0, s->upair[d + 1] = (u8)pair;
    } else if ((sm86_ops[op].kind == SM86_K_ALU || op == SM86_LDC || op == SM86_S2R || op == SM86_CS2R) &&
               op != SM86_FSETP && op != SM86_ISETP && op != SM86_DSETP && op != SM86_HSETP2 && op != SM86_R2P) {
        track_write(c, s, 0, d, key, exact, dyn, (u16)op, always);
        if (d < SM86_RZ)
            s->rtab[d] = (u8)tab, s->rtabx[d] = (u8)tabx, s->rcell[d] = cellm, s->rone[d] = (u8)one,
                      s->rpair[d] = (u8)pair;
        if (wide && d + 1 < SM86_RZ)
            s->rkey[d + 1] = 0, s->rexact[d + 1] = 0, s->rdyn[d + 1] = (u8)dyn, s->rtab[d + 1] = (u8)tab,
                      s->rtabx[d + 1] = 0, s->rcell[d + 1] = cellm, s->rone[d + 1] = 0, s->rpair[d + 1] = (u8)pair;
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
/* where two paths meet: the keys they agree on, exact where both are; dyn where either is, or where an indirect key
   meets another (track_conflict); a table's word itself where both have it, the cells either came from */
static void track_meet(Ctx* c, u32* key, u8* exact, u8* dyn, u8* tab, u8* tabx, u32* cell, u8* one, u8* pair,
                       const u32* key2, const u8* exact2, const u8* dyn2, const u8* tab2, const u8* tabx2,
                       const u32* cell2, const u8* one2, const u8* pair2, u32 n) {
    for (u32 r = 0; r < n; r++) {
        one[r] &= one2[r];
        if (pair[r] != pair2[r]) pair[r] = 0; /* then compared with every buffer */
        tabx[r] &= tabx2[r] & (tab[r] == tab2[r]);
        cell[r] |= cell2[r];
        if (!tab[r]) tab[r] = tab2[r]; /* a pointer from a table on either path; two tables keep the first */
        if (key[r] != key2[r]) {
            if (key_indirect(key[r]) || key_indirect(key2[r])) {
                track_conflict(c, key[r], key2[r]);
                dyn[r] = 1;
            }
            key[r] = 0, exact[r] = 0;
        } else
            exact[r] &= exact2[r];
        dyn[r] |= dyn2[r];
    }
}
static int track_globals(Ctx* c) {
    u32 n = c->n, nb = 0;
    c->addr_key = calloc(4 * ((size_t)n + 1), sizeof *c->addr_key);
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
                track_meet(c, s->rkey, s->rexact, s->rdyn, s->rtab, s->rtabx, s->rcell, s->rone, s->rpair,
                           out[p].rkey, out[p].rexact, out[p].rdyn, out[p].rtab, out[p].rtabx, out[p].rcell,
                           out[p].rone, out[p].rpair, 256);
                track_meet(c, s->ukey, s->uexact, s->udyn, s->utab, s->utabx, s->ucell, s->uone, s->upair,
                           out[p].ukey, out[p].uexact, out[p].udyn, out[p].utab, out[p].utabx, out[p].ucell,
                           out[p].uone, out[p].upair, 64);
            }
            if (!preds) continue; /* nothing reaches it yet */
            for (u32 i = first; i < start[b + 1]; i++) track_step(c, i, s);
            if (!reached[b] || memcmp(&out[b], s, TRACK_KEYS)) changed = 1;
            out[b] = *s;
            reached[b] = 1;
        }
    }
    /* an address at the sum of two bases (track_step's pairs) binds both of them. Where that would take the program
       past its 16 buffer slots, the base is the product's factor, the 64-bit address of a pair of constant buffer
       words: the other is the offset in every program of one game that adds two (c[1][0] or c[1][8], 0 as it runs) */
    if (ok && c->npairs) {
        u32 seen[24], nseen = 0;
        for (u32 i = 0; i < n + c->ndyn_keys + 2 * c->npairs; i++) {
            u32 key = i < n                   ? c->addr_key[i]
                      : i < n + c->ndyn_keys ? c->dyn_keys[i - n] + 1
                                              : c->pairs[(i - n - c->ndyn_keys) / 2][(i - n - c->ndyn_keys) & 1],
                dup = 0;
            if (!key) continue;
            for (u32 k = 0; k < nseen; k++) dup |= seen[k] == key;
            if (!dup && nseen < 24) seen[nseen++] = key;
        }
        if (nseen > 16)
            for (u32 i = 0; i < n; i++) {
                u32 p = c->addr_key[3 * (n + 1) + i];
                if (!p || c->addr_key[i]) continue;
                c->addr_key[i] = c->pairs[p - 1][1];
                c->addr_key[2 * (n + 1) + i] = 0;
                c->addr_key[3 * (n + 1) + i] = 0;
            }
    }
    /* each pointer table's windows: the buffer slots the program's other buffers leave, shared among its tables */
    if (ok && c->table_banks) {
        c->si->table_cells = c->table_cells;
        u32 others[16], nothers = 0, ntables = snx_popcount64(c->table_banks);
        for (u32 i = 0; i < n + c->ndyn_keys + 2 * c->npairs; i++) {
            u32 key = i < n                   ? c->addr_key[i]
                      : i < n + c->ndyn_keys ? c->dyn_keys[i - n] + 1
                                              : c->pairs[(i - n - c->ndyn_keys) / 2][(i - n - c->ndyn_keys) & 1],
                seen = 0;
            if (!key) continue;
            for (u32 k = 0; k < nothers; k++) seen |= others[k] == key;
            if (!seen && nothers < 16) others[nothers++] = key;
        }
        u32 windows = nothers + ntables <= 16 ? (16 - nothers) / ntables : 1;
        if (windows > SH_GBUF_TABLE_MAX) windows = SH_GBUF_TABLE_MAX;
        for (u32 b = 0; b < 18; b++)
            for (u32 w = 0; (c->table_banks >> b & 1) && w < windows && c->ndyn_keys < 16; w++)
                c->dyn_keys[c->ndyn_keys++] = SH_GBUF_TABLE_KEY(b, w);
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
    u32 none = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, first, cu(c, ~0u));
    u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, word, offset);
    u32 page = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, at, cu(c, 10));
    u32 entry =
        spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, none, c->zero, first),
                          spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, none, c->zero, page));
    u32 mapped =
        spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                        spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, smap_word(c, entry), cu(c, 10)),
                        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, at, cu(c, 1023)));
    mapped = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, word, count), mapped, cu(c, ~0u));
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, none, word, mapped);
}
/* a word of global (buffer), shared (buffer 0) or local (MEM_LOCAL) memory at the byte address. An address past
   the local memory wraps in it, as spirv_translate.c keeps Maxwell's LDL and STL in theirs */
static u32 mem_ptr(Ctx* c, u32 buffer, u32 address, u32 word) {
    u32 index = spv_emit_iadd_u32(
        &c->e, c->t_u32, spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2)), cu(c, word));
    if (buffer == MEM_LOCAL)
        return spv_emit_function_element_pointer(
            &c->e, c->local_ptr, c->local,
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, index, cu(c, c->local_words - 1)));
    if (buffer)
        return spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, buffer, c->zero, gbuf_word_index(c, buffer, index));
    u32 last = cu(c, c->shared_words - 1);
    index = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, last, index), last, index);
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, c->shared_ptr, shared_var(c), index);
}
/* an address in shared or local memory: the register at 24 times 1, 4, 8 or 16 (bits 78-79, NAK's OffsetStride), the
   uniform register ur (SM86_URZ for none) and the signed 24-bit offset */
static u32 stack_address(Ctx* c, const Sm86Insn* in, u32 ur) {
    static const u8 shifts[4] = {0, 2, 3, 4};
    u32 imm = sm86_field(in, 40, 24);
    u32 bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    u32 base = rd(c, sm86_field(in, 24, 8)), shift = shifts[sm86_field(in, 78, 2)];
    if (shift) base = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, base, cu(c, shift));
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
/* a word of global (buffer), shared (buffer 0) or local (MEM_LOCAL) memory: in a module of the Vulkan memory model, a
   global word through a relaxed atomic (gbuf_slot's coherence) and a shared one with NonPrivatePointer (which the
   barriers order for the workgroup's other invocations) */
static u32 mem_word_load(Ctx* c, u32 buffer, u32 ptr) {
    if (buffer && buffer != MEM_LOCAL && c->coop) return spv_emit_atomic_load_u32(&c->e, c->t_u32, ptr);
    return buffer ? spv_emit_load(&c->e, c->t_u32, ptr) : spv_emit_load_nonprivate(&c->e, c->t_u32, ptr);
}
static void mem_word_store(Ctx* c, u32 buffer, u32 ptr, u32 value) {
    if (buffer && buffer != MEM_LOCAL && c->coop)
        spv_emit_atomic_store_u32(&c->e, c->t_u32, ptr, value);
    else if (buffer)
        spv_emit_store(&c->e, ptr, value);
    else
        spv_emit_store_nonprivate(&c->e, ptr, value);
}
/* a load of mem_type (NAK's MemType) into registers dst on */
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst) {
    u32 words = type == 5 ? 2 : type == 6 ? 4 : 1;
    for (u32 k = 0; k < words; k++) {
        u32 v = mem_word_load(c, buffer, mem_ptr(c, buffer, address, k));
        if (type < 4) {
            /* a byte or a halfword of the word, sign or zero extended */
            u32 shift = spv_emit_binary(
                &c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, address, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
            u32 bits = cu(c, type < 2 ? 8 : 16);
            v = type & 1 ? to_u(c, spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                                                    spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, v), shift, bits))
                         : spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, v, shift, bits);
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
        u32 shift = spv_emit_binary(
            &c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, address, cu(c, type < 2 ? 3 : 2)), cu(c, 3));
        u32 mask = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, cu(c, (1u << width) - 1), shift);
        u32 bits =
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                            spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, rd(c, data), shift), mask);
        u32 ptr = mem_ptr(c, buffer, address, 0);
        if (buffer == MEM_LOCAL) /* the invocation's own: no other can store to the word */
            spv_emit_store(&c->e, ptr,
                           spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                           spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                                                           spv_emit_load(&c->e, c->t_u32, ptr),
                                                           spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, mask)),
                                           bits));
        else if (buffer) {
            spv_emit_atomic_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_AND, ptr,
                                spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, mask));
            spv_emit_atomic_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_OR, ptr, bits);
        } else {
            spv_emit_atomic_shared_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_AND, ptr,
                                       spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, mask));
            spv_emit_atomic_shared_u32(&c->e, c->t_u32, SPV_OP_ATOMIC_OR, ptr, bits);
        }
        return;
    }
    u32 words = type == 5 ? 2 : type == 6 ? 4 : 1;
    for (u32 k = 0; k < words; k++)
        mem_word_store(c, buffer, mem_ptr(c, buffer, address, k), rd(c, data >= SM86_RZ ? data : data + k));
}

/* RED.ADD.F32 and ATOMG.ADD.F32 (NAK's AtomType F32, .FTZ.RN): a float atomic add to the word of a global buffer at
   the byte address, the word before it returned. With the device's shaderBufferFloat32AtomicAdd one OpAtomicFAddEXT
   through the buffer's view as floats (the same binding, Aliased as the words'). Else a loop of compare and exchange
   on the word's bits, which ends once no other invocation stored to the word between its load and its exchange: one
   of the lanes that race on a word succeeds each time round. Its sum flushes subnormals as the instruction does
   (ftz) */
static u32 atomic_fadd(Ctx* c, u32 buffer, u32 address, u32 data) {
    if (g_shader_float_atomic_add) {
        u32 slot = 0;
        while (slot < 15 && c->gbuf[slot] != buffer) slot++;
        if (!c->gbuf_f32[slot]) {
            u32 structure = spv_emit_type_struct_one(&c->e, spv_emit_type_runtime_array(&c->e, c->t_f32, 4));
            c->gbuf_f32[slot] =
                spv_emit_storage_buffer(&c->e, structure, (u32)shader_vk_set(c->stage), SNX_VKB_SSBO + slot);
            spv_emit_decorate(&c->e, c->gbuf_f32[slot], 20 /* Aliased */, NULL, 0);
            if (!c->coop) spv_emit_decorate(&c->e, c->gbuf_f32[slot], 23 /* Coherent */, NULL, 0);
            add_interface(c, c->gbuf_f32[slot]);
        }
        spv_emit_extension(&c->e, "SPV_EXT_shader_atomic_float_add");
        spv_emit_capability(&c->e, 6033 /* AtomicFloat32AddEXT */);
        u32 index =
            gbuf_word_index(c, buffer, spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2)));
        u32 ptr = spv_emit_storage_access_u32(&c->e, spv_emit_type_storage_pointer(&c->e, c->t_f32), c->gbuf_f32[slot],
                                              c->zero, index);
        return to_u(c, SPV(&c->e, 6035 /* OpAtomicFAddEXT */, c->t_f32, ptr, cu(c, 1) /* Device */,
                           cu(c, 0) /* Relaxed */, to_f(c, data)));
    }
    SpvEmitter* e = &c->e;
    u32 ptr = mem_ptr(c, buffer, address, 0), header = spv_emit_id(e), body = spv_emit_id(e), cont = spv_emit_id(e),
        merge = spv_emit_id(e);
    spv_emit_store(e, c->cas_var, mem_word_load(c, buffer, ptr));
    spv_emit_branch(e, header);
    spv_emit_label(e, header);
    spv_emit_loop_merge(e, merge, cont);
    spv_emit_branch(e, body);
    spv_emit_label(e, body);
    u32 old = spv_emit_load(e, c->t_u32, c->cas_var);
    u32 sum =
        nocontract(c, spv_emit_binary(e, SPV_OP_F_ADD, c->t_f32, ftz(c, to_f(c, old), 1), ftz(c, to_f(c, data), 1)));
    u32 seen = spv_emit_atomic_compare_exchange_u32(e, c->t_u32, ptr, to_u(c, ftz(c, sum, 1)), old);
    spv_emit_store(e, c->cas_var, seen);
    spv_emit_branch_conditional(e, spv_emit_binary(e, SPV_OP_I_EQUAL, c->t_bool, seen, old), merge, cont);
    spv_emit_label(e, cont);
    spv_emit_branch(e, header);
    spv_emit_label(e, merge);
    return old;
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
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, index, cu(c, words));
    u32 v =
        spv_emit_load(&c->e, c->t_u32,
                      isbe_ptr(c, var, storage, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, inside, index, last)));
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, inside, v, c->zero);
}
/* value into word index of an ISBE array of words: a store past its end goes nowhere */
static void isbe_store(Ctx* c, u32 var, u32 storage, u32 words, u32 index, u32 value) {
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, index, cu(c, words));
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
    /* an indirect draw's first group: the record's, in the uniform buffer at SNX_VKB_RS (SH_PUSH_GPU_MESH_FIRST) */
    if (!c->mesh_first) {
        c->mesh_first = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                                    SNX_VKB_RS, 1, &c->cbuf_ptr);
        add_interface(c, c->mesh_first);
    }
    u32 flags = to_u(c, push_component(c, 2));
    u32 gpu = spv_emit_binary(
        &c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, flags, cu(c, SH_PUSH_GPU_MESH_FIRST)), c->zero);
    u32 first = spv_emit_ternary(
        &c->e, SPV_OP_SELECT, c->t_u32, gpu,
        spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mesh_first, c->zero, c->zero, c->zero, c->t_u32),
        to_u(c, push_component(c, 0)));
    return spv_emit_iadd_u32(&c->e, c->t_u32, id, first);
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
    u32 word = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2));
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
                   spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, data, cu(c, 0xff)));
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

/* whether the instruction at k goes on to k + 1: not after a BRA, BRX or EXIT that always leaves (its operand too,
   sm86_always), nor a RET */
static int falls_through(const Sm86Insn* in) {
    return !((in->op == SM86_BRA || is_brx(in->op) || in->op == SM86_EXIT) && sm86_always(in)) && in->op != SM86_RET;
}
static u32 brx_targets(Ctx* c, u32 i, u32* targets, u32 max);
/* the instructions that jump to each one, build_structure's edges: a BRA's target, a BRX's cases. Returns 0 when it
   can't be made */
static int flow_sources(Ctx* c) {
    if (c->jump_first) return 1;
    u32 n = c->n, total = 0, t[64];
    for (u32 i = 0; i < n; i++) {
        const Sm86Insn* in = &c->in[i];
        if (sm86_never(in)) continue;
        if (in->op == SM86_BRA)
            total++;
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
            if (sm86_never(in)) continue;
            u32 count = 0;
            if (in->op == SM86_BRA)
                t[count++] = sm86_branch_target(in, i);
            else if (is_brx(in->op))
                count = brx_targets(c, i, t, 64);
            for (u32 k = 0; k < count; k++) {
                if (t[k] >= n) continue;
                if (pass == 0)
                    first[t[k] + 1]++;
                else
                    from[first[t[k]] + fill[t[k]]++] = i;
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
        case SM86_BSSY:
        case SM86_BSYNC:
        case SM86_BREAK:
        case SM86_WARPSYNC:
        case SM86_KILL:
        case SM86_BAR:
        case SM86_NOP:
        case SM86_ERRBAR:
        case SM86_DEPBAR:
        case SM86_NANOSLEEP:
        case SM86_MEMBAR:
        case SM86_CCTL:
        case SM86_STG:
        case SM86_STG_NOU:
        case SM86_STL:
        case SM86_STL_NOU:
        case SM86_STS:
        case SM86_STS_NOU:
        case SM86_RED:
        case SM86_AST:
        case SM86_SUST_P:
        case SM86_SUST_D:
        case SM86_SUST_P_B:
        case SM86_SUST_D_B:
        case SM86_ULDC:
        case SM86_ULDC_IDX:
        case SM86_ULDCG:
        case SM86_S2UR:
        case SM86_R2UR:
        case SM86_PLOP3:
        case SM86_UPLOP3:
        case SM86_UVOTE:
        case SM86_ISETP:
        case SM86_FSETP:
        case SM86_DSETP:
        case SM86_HSETP2:
        case SM86_R2P:
        case SM86_BMOV_B:
        case SM86_BMOV_BI:
        case SM86_BMOV_BL:
        case SM86_ISBEWR: return 0;
        case SM86_TEX:
        case SM86_TEX_B:
        case SM86_TLD:
        case SM86_TLD_B:
        case SM86_TLD4:
        case SM86_TLD4_B:
        case SM86_TMML:
        case SM86_TMML_B:
        case SM86_TXD:
        case SM86_TXD_B:
        case SM86_TXQ:
        case SM86_TXQ_B:
        case SM86_TEX_U:
        case SM86_TLD_U:
        case SM86_TLD4_U:
        case SM86_TXQ_U: {
            u32 d1 = sm86_field(in, 64, 8);
            if (d1 < SM86_RZ && r >= d1 && r < d1 + 4) return 1;
            return in->dst < SM86_RZ && r >= in->dst && r < in->dst + 4u;
        }
        case SM86_LDG:
        case SM86_LDG_NOU:
        case SM86_LDL:
        case SM86_LDS:
        case SM86_LDC:
        case SM86_LDC_BINDLESS:
        case SM86_ALD:
        case SM86_ATOMG:
        case SM86_ATOMG_NOU:
        case SM86_ATOMG_CAS:
        case SM86_ATOMS:
        case SM86_ATOMS_NOU:
        case SM86_ATOMS_CAS:
        case SM86_SULD_P:
        case SM86_SULD_P_B:
        case SM86_SULD_D:
        case SM86_SULD_D_B:
        case SM86_SURED:
        case SM86_SURED_B:
        case SM86_SUATOM:
        case SM86_SUATOM_B:
        case SM86_SUATOM_CAS_B:
        case SM86_HMMA:
        case SM86_LDSM: return in->dst < SM86_RZ && r >= in->dst && r < in->dst + 4u;
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
    return spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                            spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, x), cu(c, k * bits), cu(c, bits));
}
/* the offset a texture instruction packs in a register, as the integer vector of dims components SPIR-V takes (a
   1D texture's y, bound as 2D, is 0): constant when known */
static u32 offset_vector(Ctx* c, int known, u32 value, u32 x, u32 dims, u32 used, u32 bits) {
    u32 v[3];
    for (u32 k = 0; k < dims; k++)
        v[k] = k < used ? offset_part(c, known, value, x, k, bits) : spv_emit_constant_u32(&c->e, c->t_i32, 0);
    u32 type = spv_emit_type_vector(&c->e, c->t_i32, dims);
    if (known)
        return dims == 2 ? spv_emit_constant_vec2(&c->e, type, v[0], v[1])
                         : spv_emit_constant_vec3(&c->e, type, v[0], v[1], v[2]);
    return dims == 2 ? spv_emit_vec2_construct(&c->e, type, v[0], v[1])
                     : spv_emit_vec3_construct(&c->e, type, v[0], v[1], v[2]);
}

/* the texture slot of a handle at byte offset off of a bank */
/* the texture slot of the handle at c[bank][off] with that view: ms for a multisampled 2D texture (ShTex.flags bit 1,
   which the renderer binds a multisampled view for) */
static int tex_slot(Ctx* c, u32 bank, u32 off, u32 type, u32 array, u32 shadow, u32 ms) {
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ntex; i++) {
        const ShTex* t = &si->tex[i];
        if (t->bl_cbuf == bank && t->cbuf_off == off / 4 && t->type == type && t->array == array &&
            t->shadow == shadow && !!(t->flags & 2) == !!ms && !(t->flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY)))
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
                                                       (int)array, 0, g_shader_tex_array, &c->tex_sampled[i]);
        add_interface(c, c->tex_array[i]);
        if (shadow) {
            c->tex_depth[i] = spv_emit_sampler_image_array(&c->e, c->t_f32, c->t_u32, set, SNX_VKB_TEXDEPTH, dim,
                                                           (int)array, 1, g_shader_tex_array, &c->tex_depth_type[i]);
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
                                      spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, index, cu(c, 2)),
                                      spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, index, cu(c, 3)), c->t_u32);
}
/* For this instruction, an indexed slot's tex[] is the element its LDC chose, clamped to the array, as
   shader_direct_swap_element does. Returns the array's element */
static u32 indexed_element(Ctx* c, int slot, u32 var) {
    u32 limit = cu(c, SNX_TEX_ARRAY - 1), index = spv_emit_load(&c->e, c->t_u32, var);
    index = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, limit, index), limit, index);
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
    u32 func = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, meta, cu(c, 7)), one = cf(c, 1.0f),
        none = cf(c, 0.0f);
    u32 result = none;
    for (u32 i = 0; i < 8; i++) {
        u32 pass = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                    spv_emit_fcompare(&c->e, c->t_bool, condition[i], dref, color), one, none);
        result = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                  spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, func, cu(c, i)), pass, result);
    }
    u32 view = spv_emit_sampler_array_element(&c->e, c->tex_depth_type[slot], c->tex_depth[slot], element);
    u32 depth =
        spv_emit_sample_image(&c->e, c->t_f32, c->tex_depth_type[slot], view, coord, dref, mode, level, offset, 0);
    u32 is_depth = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool,
                                   spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, meta, cu(c, 8)), cu(c, 8));
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, is_depth, depth, result);
}
/* a filtered sample of slot at coord (array: its last component the layer) moved by const_offset (0 for none) as the
   least or greatest of its texels (reduce: shader_minmax_slot), as its sampler asks and the device's can't */
static u32 minmax_sample(Ctx* c, int slot, u32 coord, int array, u32 const_offset, int reduce) {
    ShMinmaxTypes t = {c->t_f32, c->t_u32, c->t_bool, c->t_v4f, c->glsl};
    return shader_minmax_sample(&c->e, &t, c->tex_sampled[slot], c->tex[slot], coord, array, const_offset, 0,
                                reduce == 2);
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
    /* a handle from memory (TEX and TLD4 with it in a uniform register) has its slot from memory_handles and none in
       the operand list, as a constant buffer handle */
    int memory = in->op == SM86_TEX_U || in->op == SM86_TLD4_U;
    int bindless = in->op == SM86_TEX_B, op = bindless || in->op == SM86_TEX_U ? SM86_TEX
                                              : in->op == SM86_TLD4_U          ? SM86_TLD4
                                                                               : in->op;
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
    int slot = bindless || memory ? (int)c->handle[c->cur].slot - 1
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
        f[count++] =
            spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32,
                           spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, rd(c, regs[0]), cu(c, 0xffff)));
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
            result =
                spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, comp, dref, o);
        } else {
            for (u32 k = 0; k < 4; k++) {
                u32 word = rd(c, off_reg >= SM86_RZ ? off_reg : off_reg + k / 2);
                u32 type2 = spv_emit_type_vector(&c->e, c->t_i32, 2);
                u32 o = spv_emit_vec2_construct(&c->e, type2, offset_part(c, 0, 0, word, (k % 2) * 2, 8),
                                                offset_part(c, 0, 0, word, (k % 2) * 2 + 1, 8));
                v[k] = spv_emit_vec4_extract(
                    &c->e, c->t_f32,
                    spv_emit_image_gather_ex(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, comp, dref, o),
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
            v[k] = to_f(
                c, spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32,
                                  spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32,
                                                  spv_emit_vec4_extract(&c->e, c->t_f32, lod, 1 - k), cf(c, 256.0f))));
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
                        spv_emit_vec4_extract(
                            &c->e, c->t_f32,
                            spv_emit_image_query_lod(&c->e, c->t_v2f, c->tex_sampled[slot], c->tex[slot], coord), 0));
                }
                spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
                u32 size = spv_emit_image_query_size_lod(&c->e, spv_emit_type_vector(&c->e, c->t_i32, dims + array),
                                                         c->tex_sampled[slot], c->tex[slot],
                                                         spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32, at));
                u32 moved[4];
                for (u32 k = 0; k < count; k++) {
                    moved[k] = spv_emit_vec4_extract(&c->e, c->t_f32, coord, k);
                    if (k < used)
                        moved[k] = spv_emit_binary(
                            &c->e, SPV_OP_F_ADD, c->t_f32, moved[k],
                            spv_emit_binary(&c->e, SPV_OP_F_DIV, c->t_f32,
                                            spv_emit_unary(&c->e, SPV_OP_CONVERT_S_TO_F, c->t_f32,
                                                           spv_emit_vec4_extract(&c->e, c->t_i32, o, k)),
                                            spv_emit_unary(&c->e, SPV_OP_CONVERT_S_TO_F, c->t_f32,
                                                           spv_emit_vec4_extract(&c->e, c->t_i32, size, k))));
                }
                coord = coord_vector(c, moved, count);
            }
        }
        int reduce = shadow ? 0 : shader_minmax_slot(c->si->tex_minmax_mask, c->si->tex_minmax_max, slot);
        result = indexed && shadow ? sample_indexed_shadow(c, slot, element, coord, dref, mode, level, const_offset)
                 : reduce ? minmax_sample(c, slot, coord, (int)array, const_offset, reduce)
                          : spv_emit_sample_image(&c->e, rtype, c->tex_sampled[slot], c->tex[slot], coord, dref, mode,
                                                  level, const_offset, 0);
        if (shadow) {
            u32 v[4] = {result, cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
            result = spv_emit_vec4_construct(&c->e, c->t_v4f, v);
        }
    }
    tex_results(c, in, result, c->t_f32);
}
static int writes_uniform(const Sm86Insn* w, u32 r);
/* A texture instruction whose handle is in a uniform register (TEX_U and the others): refused, with where the handle
   comes from, the writers of the register followed back through UMOV and R2UR in program order. Only a handle in a
   constant buffer has a slot the renderer binds (tex_slot): one game's are an LDG.64 of a table of handles in memory
   each, the texture chosen at run time, which a slot can't name */
static void uniform_handle(Ctx* c, const Sm86Insn* in) {
    u32 r = sm86_field(in, 40, 6), uniform = 1, at = c->cur;
    const char* from = "nothing";
    for (u32 depth = 0; depth < 8 && r < (uniform ? SM86_URZ : SM86_RZ); depth++) {
        int k = -1;
        for (u32 j = at; j-- > 0 && k < 0;) {
            /* a register's: of the other instructions than loads and the ALU's, the one named alone (may_write takes
               an IPA's for two) */
            const Sm86Insn* w = &c->in[j];
            int loads = w->op == SM86_LDG || w->op == SM86_LDG_NOU || w->op == SM86_LDS || w->op == SM86_LDL ||
                        w->op == SM86_LDC || sm86_ops[w->op].kind == SM86_K_ALU;
            if (uniform ? writes_uniform(w, r) : writes_word(w, r) && (loads || w->dst == r)) k = (int)j;
        }
        if (k < 0) break;
        const Sm86Insn* w = &c->in[k];
        from = sm86_ops[w->op].name;
        at = (u32)k;
        if (w->op == SM86_UMOV && w->b.kind == SM86_SRC_UREG)
            r = w->b.reg;
        else if (w->op == SM86_R2UR)
            r = sm86_field(w, 24, 8), uniform = 0;
        else if (w->op == SM86_MOV && w->b.kind == SM86_SRC_REG)
            r = w->b.reg;
        else
            break;
    }
    fail(c, "%s whose handle comes from %s", sm86_ops[in->op].name, from);
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
        f[count++] = spv_emit_unary(
            &c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32,
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, rd(c, TXD_REG(a, coords)), cu(c, 0xffff)));
#undef TXD_REG
    int reduce = shader_minmax_slot(c->si->tex_minmax_mask, c->si->tex_minmax_max, slot);
    u32 coord = coord_vector(c, f, count);
    tex_results(c, in,
                reduce ? minmax_sample(c, slot, coord, (int)array, 0, reduce)
                       : spv_emit_sample_grad(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord, gx, gy, 0),
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
    int slot = in->op == SM86_TLD_U ? (int)c->handle[c->cur].slot - 1 /* memory_handles' */
                                    : tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, 0, ms);
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
                                     spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_u32,
                                                    offset_part(c, 0, 0, rd(c, regs[n0 + (lod_mode == 3)]), k, 4)));
    if (c->stage == ST_FS && type == 1) scaled_fetch(c, slot, &v[0], &v[1]);
    if (array) v[count++] = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, rd(c, regs[0]), cu(c, 0xffff));
    u32 ivec = spv_emit_type_vector(&c->e, c->t_i32, count);
    u32 uvec = spv_emit_type_vector(&c->e, c->t_u32, count);
    u32 coord = spv_emit_unary(&c->e, SPV_OP_BITCAST, ivec,
                               count == 2 ? spv_emit_vec2_construct(&c->e, uvec, v[0], v[1])
                                          : spv_emit_vec3_construct(&c->e, uvec, v[0], v[1], v[2]));
    u32 level = lod_mode == 3 ? spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, rd(c, regs[n0]))
                              : spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, c->zero);
    u32 texel =
        ms ? spv_emit_image_fetch_sample(&c->e, c->t_v4f, c->tex_sampled[slot], c->tex[slot], coord,
                                         spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, rd(c, regs[n0 + offset])))
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
    int slot = in->op == SM86_TXQ_U ? (int)c->handle[c->cur].slot - 1 : -1; /* memory_handles' */
    for (int i = 0; i < c->si->ntex && slot < 0 && in->op != SM86_TXQ_U; i++)
        if (c->si->tex[i].bl_cbuf == bank && c->si->tex[i].cbuf_off == off &&
            !(c->si->tex[i].flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY)))
            slot = i;
    /* a TLD of a multisampled 2D texture with the handle, anywhere in the program, names its view */
    u32 ms = 0;
    for (u32 k = 0; k < c->n && slot < 0 && !ms && in->op != SM86_TXQ_U; k++)
        ms = c->in[k].op == SM86_TLD && sm86_field(&c->in[k], 78, 1) && sm86_field(&c->in[k], 40, 19) == handle;
    if (slot < 0 && in->op != SM86_TXQ_U) slot = tex_slot(c, bank, off * 4, 1, 0, 0, ms);
    if (slot < 0) return;
    const ShTex* t = &c->si->tex[slot];
    ms = t->flags & 2;
    u32 dims = (t->type == 2 ? 3 : 2) + t->array; /* a 1D texture is a 2D one, a cube's size is its face's */
    if (t->type == 3) dims = 2 + t->array;
    spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
    /* a multisampled texture has one level, and its size takes none */
    u32 size = spv_emit_image_query_size_lod(
        &c->e, spv_emit_type_vector(&c->e, c->t_u32, dims), c->tex_sampled[slot], c->tex[slot],
        ms ? 0 : spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, rd(c, sm86_field(in, 24, 8))));
    u32 v[4];
    for (u32 k = 0; k < 3; k++) v[k] = k < dims ? SPV(&c->e, SPV_OP_COMPOSITE_EXTRACT, c->t_u32, size, k) : cu(c, 1);
    if (t->type == 0) v[1] = cu(c, 1);
    if (c->stage == ST_FS && t->type == 1) scaled_size(c, slot, &v[0], &v[1]);
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
           renderer's key, bit slot of img_int_mask for a format of the other class, gives a float image (0) or an
           unsigned one (2), and of img_sint_mask a signed one (3) */
        u32 kind = atomic ? 1 : si->img_int_mask & (1u << slot) ? 0 : 2;
        if (!atomic && (si->img_sint_mask & (1u << slot))) kind = 3;
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
    u32 coord = spv_emit_unary(&c->e, SPV_OP_BITCAST, ivec,
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
                f[k] = spv_emit_unary(&c->e, SPV_OP_BITCAST, scalar, rd(c, data >= SM86_RZ ? data : data + next++));
            else
                f[k] = kind == 0 ? cf(c, k == 3 ? 1.0f : 0.0f)
                                 : spv_emit_unary(&c->e, SPV_OP_BITCAST, scalar, cu(c, k == 3));
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
static u32 warp_lane(Ctx* c) {
    return spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, 31));
}
/* value as lane j (0-31) of the invocation's own 32 holds it */
static u32 warp_read(Ctx* c, u32 value, u32 j) {
    u32 own = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, ~31u));
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
    return SPV(&c->e, 345 /* OpGroupNonUniformShuffle */, c->t_u32, cu(c, 3), value,
               spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, own, j));
}
/* value as lane l ^ mask of the invocation's own 32 holds it, for lane l (mask below 32) */
static u32 warp_xor(Ctx* c, u32 value, u32 mask) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
    return SPV(&c->e, 346 /* OpGroupNonUniformShuffleXor */, c->t_u32, cu(c, 3), value, cu(c, mask));
}
/* value as lane 4g + p of the invocation's own 32 holds it, g4 = 4g for the reader's own g: a quad broadcast where the
   stage has quad operations, which AMD makes a DPP move where a shuffle goes through LDS. Vulkan's quad is four
   invocations of adjacent subgroup ids, the first a multiple of 4, in every stage that has them; so in a subgroup of
   64 too the reader's quad is lanes 4g to 4g + 3 of its own 32, and quad lane p is lane 4g + p */
static u32 quad_read(Ctx* c, u32 value, u32 g4, u32 p) {
    if (c->stage == ST_CS ? g_shader_quad_compute : c->stage == ST_FS && g_shader_quad) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
        spv_emit_subgroup_quad_capability(&c->e);
        return spv_emit_subgroup_quad_u32(&c->e, c->t_u32, value, 0, cu(c, p));
    }
    return warp_read(c, value, spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, g4, cu(c, p)));
}
static u32 half_pair(Ctx* c, u32 bits) { return SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, bits); }
/* A zero for an HMMA to OR its B registers with before it gathers them, a new one every 8 HMMAs, so that a driver
   doesn't keep a B fragment's 16 gathered words live from its first HMMA to its last. Mesa merges identical subgroup
   operations within a block, and one game's video upscaler multiplies one B fragment by many A ones, far apart: with
   the gathers merged, its programs wanted 420 to 1430 VGPRs, and RADV spilled around nearly every product (a scratch
   load, a wait and a store each). With them gathered again, their demand fell to 180-300, and ACO's latency estimate
   by 6 to 40 times. NumWorkgroups.x is never 0 in an invocation that runs (a dispatch of no workgroups runs none),
   so the OR keeps the value on any driver, and one that proves it 0 only goes back to the merged gathers. The 8 HMMAs
   that share one keep the reuse among neighbours (a GEMM's step of a few A and B fragments): sharing none made that
   game's programs that didn't spill 6-25% slower by the estimate, sharing among 32 brought the spills back. Compute
   only, the stage with NumWorkgroups */
static u32 hmma_zero(Ctx* c) {
    u32 group = c->hmma_count++ / 8;
    if (c->stage != ST_CS) return 0;
    u32 never = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, builtin_vec3(c, &c->num_groups, 24, 0), c->zero);
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, never, cu(c, group + 1), c->zero);
}
/* HMMA on the device's matrix unit (g_shader_hmma_coop): one OpCooperativeMatrixMulAddKHR of 16x16 f16 A and B into
   a 16x16 f32 accumulator, in place of hmma's 16 B gathers and 32 dot products. SPV_KHR_cooperative_matrix leaves
   which lane holds which element to the driver, with no query, so the fragments go through shared memory: each
   subgroup has a slot of its own (SubgroupId) in an array after the program's, where A is row-major (8 words a row
   of 16 halves), B column-major (8 words a column) and D row-major in f32 (16 words a row). The product has 16
   columns for HMMA's 8: B's columns 8-15 are whatever the 64 words after its 8 hold (the start of D's), which only
   reach D's columns 8-15, never read back. The .1688 forms' k of 8 pads to 16 with zeros in A's columns and B's rows
   8-15, as stale words there could be infinities or NaNs, and 0 times those poisons the sums. The accumulator starts
   at zero and C is added to the lane's four elements of D after the product: staging C as well (4 stores, 8 loads)
   cost one game's video upscaler half of the gain. A subgroup barrier comes before the stores (the last HMMA's
   loads of the slot), between them and the matrix loads, and between D's store and the lanes' loads of it; RADV
   makes them no instruction, a wave's LDS accesses being in order.
   The products of two f16 are exact in f32, and the device sums them in f32 inside the instruction, rounded as it
   does it (RDNA 3.5's is a few f32 places off an FMA chain even on exact sums, as its v_dot2_f32_f16 is); C then adds
   one rounding, in f32. So both D types keep hmma's bound, that of an f32 sum of the products and C, which an f16 D
   then rounds to. On a Radeon 890M (RDNA 3.5, RADV) that upscaler's 7 HMMA programs went from 46.6 ms of GPU time a
   frame to 38.7 (two of them slower, by 4% and 13%), and its movie from 12.6 frames a second to 14.2: the GPU
   gathers and dot products were most of their instructions, and the matrix unit is idle otherwise */
#define COOP_B     128u /* the words of a slot: A at 0, B, D */
#define COOP_D     192u
#define COOP_WORDS 448u
/* Whether a program's HMMAs take hmma_coop: a compute program with one, in blocks of whole subgroups of 32 (vk/
   compute.c asks for subgroups of 32 for a program that reads other lanes, full where x is a multiple of 32; a 16x16
   matrix takes the subgroup's 32 lanes), with shaderFloat16 for the halves, and with room for the slots after the
   program's shared memory within the device's (g_shader_hmma_coop). It is the module's choice, as the extension asks
   for the Vulkan memory model */
static int coop_wanted(const Ctx* c) {
    if (!g_shader_hmma_coop || !g_shader_float16 || c->stage != ST_CS || c->si->cs_local[0] <= 0 ||
        c->si->cs_local[0] % 32)
        return 0;
    u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
    u32 own = c->si->shared_size > 0 ? (u32)c->si->shared_size : 16384u; /* shared_var's */
    if (own + lanes / 32 * COOP_WORDS * 4 > (u32)g_shader_hmma_coop) return 0;
    for (u32 i = 0; i < c->n; i++)
        if (c->in[i].op == SM86_HMMA) return 1;
    return 0;
}
static void coop_barrier(Ctx* c) {
    SPV_VOID(&c->e, 224 /* OpControlBarrier */, cu(c, 3), cu(c, 3),
             cu(c, spv_emit_memory_semantics(&c->e, 0x108)) /* Subgroup, Subgroup, AcquireRelease | Workgroup */);
}
static u32 coop_ptr(Ctx* c, u32 index) {
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, spv_emit_type_workgroup_pointer(&c->e, c->t_u32), c->coop_slots, index);
}
static u32 coop_add(Ctx* c, u32 at, u32 k) { return k ? spv_emit_iadd_u32(&c->e, c->t_u32, at, cu(c, k)) : at; }
static void hmma_coop(Ctx* c, const Sm86Insn* in, int k16, int f32, u32 ra, u32 rb, u32 rc) {
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    for (k = 0; k < na; k++) a[k] = rd(c, ra + k);
    for (k = 0; k < nb; k++) b[k] = rd(c, rb + k);
    for (k = 0; k < ncd; k++) cc[k] = rd(c, rc + k);
    spv_emit_capability(&c->e, SPV_CAPABILITY_FLOAT16);
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
    spv_emit_capability(&c->e, 6022 /* CooperativeMatrixKHR */);
    spv_emit_extension(&c->e, "SPV_KHR_cooperative_matrix");
    u32 t_h = spv_emit_type_float(&c->e, 16);
    u32 t_a = spv_emit_type_cooperative_matrix(&c->e, t_h, 3 /* Subgroup */, 16, 16, 0 /* MatrixAKHR */);
    u32 t_b = spv_emit_type_cooperative_matrix(&c->e, t_h, 3, 16, 16, 1 /* MatrixBKHR */);
    u32 t_acc = spv_emit_type_cooperative_matrix(&c->e, c->t_f32, 3, 16, 16, 2 /* MatrixAccumulatorKHR */);
    if (!c->coop_slots) {
        u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
        u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, lanes / 32 * COOP_WORDS);
        c->coop_slots = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
        add_interface(c, c->coop_slots);
    }
    u32 base = spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32, builtin_u32(c, &c->subgroup_id, 40 /* SubgroupId */),
                               cu(c, COOP_WORDS));
    u32 lane = warp_lane(c);
    u32 g8 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, lane, cu(c, 2)), cu(c, 3));
    u32 t = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3));
    /* lane l's words (g = l / 4, t = l % 4): A's at 8g + t (+64 row g + 8, +4 columns 8-15), B's at 8g + t (+4 rows
       8-15), D's at 16g + 2t (+1 the next column, +128 row g + 8) */
    u32 at_a = spv_emit_iadd_u32(&c->e, c->t_u32, base, spv_emit_iadd_u32(&c->e, c->t_u32, g8, t));
    u32 at_b = coop_add(c, at_a, COOP_B);
    u32 at_d = spv_emit_iadd_u32(
        &c->e, c->t_u32, base,
        spv_emit_iadd_u32(&c->e, c->t_u32, spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, g8, cu(c, 1)),
                          spv_emit_iadd_u32(&c->e, c->t_u32,
                                            spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 1)),
                                            cu(c, COOP_D))));
    coop_barrier(c);
    static const u32 a_at[4] = {0, 64, 4, 68};
    for (k = 0; k < 4; k++)
        spv_emit_store_nonprivate(&c->e, coop_ptr(c, coop_add(c, at_a, a_at[k])), k < na ? a[k] : c->zero);
    for (k = 0; k < 2; k++)
        spv_emit_store_nonprivate(&c->e, coop_ptr(c, coop_add(c, at_b, 4 * k)), k < nb ? b[k] : c->zero);
    coop_barrier(c);
    u32 ma = SPV(&c->e, 4457 /* OpCooperativeMatrixLoadKHR */, t_a, coop_ptr(c, base), cu(c, 0) /* RowMajorKHR */,
                 cu(c, 8), 0x20 /* NonPrivatePointer */);
    u32 mb =
        SPV(&c->e, 4457, t_b, coop_ptr(c, coop_add(c, base, COOP_B)), cu(c, 1) /* ColumnMajorKHR */, cu(c, 8), 0x20);
    u32 md = SPV(&c->e, 4459 /* OpCooperativeMatrixMulAddKHR */, t_acc, ma, mb,
                 spv_emit_const_composite(&c->e, t_acc, (const u32[]){cf(c, 0.0f)}, 1));
    SPV_VOID(&c->e, 4458 /* OpCooperativeMatrixStoreKHR */, coop_ptr(c, coop_add(c, base, COOP_D)), md, cu(c, 0),
             cu(c, 16), 0x20);
    coop_barrier(c);
    u32 acc[4]; /* row g columns 2t and 2t + 1, row g + 8 the same */
    for (k = 0; k < 4; k++) {
        u32 at = coop_add(c, at_d, (k >> 1) * 128 + (k & 1));
        acc[k] = to_f(c, spv_emit_load_nonprivate(&c->e, c->t_u32, coop_ptr(c, at)));
    }
    if (rc < SM86_RZ)
        for (u32 i = 0; i < 2; i++) {
            u32 pair = f32 ? 0 : half_pair(c, cc[i]);
            for (u32 j = 0; j < 2; j++) {
                u32 addend = f32 ? to_f(c, cc[2 * i + j]) : spv_emit_vec4_extract(&c->e, c->t_f32, pair, j);
                acc[2 * i + j] = op2(c, SPV_OP_F_ADD, c->t_f32, acc[2 * i + j], addend);
            }
        }
    u32 d = in->dst;
    if (d >= SM86_RZ) return;
    if (f32)
        for (k = 0; k < 4; k++) wr(c, d + k, to_u(c, acc[k]));
    else
        for (u32 i = 0; i < 2; i++)
            wr(c, d + i,
               SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                       spv_emit_vec2_construct(&c->e, c->t_v2f, acc[2 * i], acc[2 * i + 1])));
}
/* HMMA (0x23c): D = A B + C on the warp's fragments, as PTX's mma.sync.aligned.m16n8k16 (bit 75) or m16n8k8 .row.col
   with f16 A and B, and f16 (bit 76 clear) or f32 C and D (the encodings of CuAssembler's sm_86 records: bits 72-90 8
   16816.F16, 0x18 16816.F32, 0 1688.F16, 0x10 1688.F32; BF16, TF32 and 1684 are other bits). D at 16, A at 24, B at
   32, C at 64 (RZ: zero), each the first of its registers. With g = l / 4 and t = l % 4, lane l holds: A's rows g
   (a0) and g + 8 (a1) at columns 2t and 2t + 1, and for k16 the same at columns 2t + 8 and 2t + 9 (a2, a3); B's column
   g at rows 2t and 2t + 1 (b0), for k16 also 2t + 8 and 2t + 9 (b1); C and D's rows g and g + 8 at columns 2t and
   2t + 1, a register for each element in f32 (c0 c1 row g, c2 c3 row g + 8) or a pair for each row in f16. Each lane
   gathers the A pairs of its two rows from its quad, lanes 4g to 4g + 3 (quad_read), and the B pairs of its two
   columns from lanes 8t + p and 8t + 4 + p (16 shuffles each for k16, of the B registers OR'd with hmma_zero), and
   sums the products in f32, which an f16 D then rounds to. A product of two f16 is exact in f32, so only the sums
   round, in f32 for both D types: with the mixed dot product (g_shader_mixed_dot) each pair of products goes into
   the accumulator in one instruction, rounded as the device does it (RDNA 3.5's is a few f32 places off an FMA chain
   even on exact sums, within the same bound); without it the pairs are unpacked to f32 and dotted.
   With g_shader_hmma_pairs, lanes l and l ^ 4 (quads g and g ^ 1, the same t, so the same two B columns) split the
   16 B words both multiply by: lane l shuffles in those of quad lanes 2 G0 and 2 G0 + 1 (G0 = bit 2 of l) and takes
   its pair's, the other two, with a xor of 4, which RADV makes a DPP move where a shuffle at any index is a permute
   through LDS. Its A pairs follow the same quad lanes, in the same order: quad broadcasts of lanes u and 2 + u, the
   one G0 picks. Only the order of the sums changes, lanes of G0 1 adding quad lanes 2 and 3's pairs first */
static void hmma(Ctx* c, const Sm86Insn* in) {
    u32 v = sm86_field(in, 72, 19);
    if (v & ~0x18u) {
        fail(c, "HMMA variant %#x", v);
        return;
    }
    int k16 = (int)(v >> 3 & 1), f32 = (int)(v >> 4 & 1);
    u32 ra = sm86_field(in, 24, 8), rb = sm86_field(in, 32, 8), rc = sm86_field(in, 64, 8);
    if (c->coop) {
        hmma_coop(c, in, k16, f32, ra, rb, rc);
        return;
    }
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    for (k = 0; k < na; k++) a[k] = ra >= SM86_RZ ? c->zero : rd(c, ra + k);
    u32 zero = rb >= SM86_RZ ? 0 : hmma_zero(c);
    for (k = 0; k < nb; k++) {
        b[k] = rb >= SM86_RZ ? c->zero : rd(c, rb + k);
        if (zero) b[k] = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, b[k], zero);
    }
    for (k = 0; k < ncd; k++) cc[k] = rc >= SM86_RZ ? c->zero : rd(c, rc + k);
    u32 lane = warp_lane(c);
    u32 g4 = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 28));
    u32 t8 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3)), cu(c, 3));
    /* the pairs as two f16 for OpFDot2MixAcc32VALVE (a bitcast: no instruction), or unpacked to two f32 */
    int mixed = g_shader_mixed_dot && g_shader_float16;
    u32 t_v2h = 0;
    if (mixed) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_FLOAT16);
        spv_emit_capability(&c->e, 6912 /* DotProductFloat16AccFloat32VALVE */);
        spv_emit_extension(&c->e, "SPV_VALVE_mixed_float_dot_product");
        t_v2h = spv_emit_type_vector(&c->e, spv_emit_type_float(&c->e, 16), 2);
    }
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
    int pairs = g_shader_hmma_pairs != 0;
    u32 g0 = 0, t8h = 0, mine[2][2][2]; /* pairs: [column 2t + j][B's register h][quad lane 2 G0 + u]'s word */
    if (pairs) {
        u32 half = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                                   spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, lane, cu(c, 1)),
                                   cu(c, 2)); /* 2 G0 */
        g0 = spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool, half, c->zero);
        t8h = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, t8, half);
    }
    for (u32 p = 0; p < 4; p++) { /* pairs: quad lanes 2 G0 + p, then 2 (1 - G0) + p - 2 */
        u32 av[4], bv[2][2];
        for (k = 0; k < na; k++) {
            u32 bits;
            if (pairs) {
                u32 x = quad_read(c, a[k], g4, p & 1), y = quad_read(c, a[k], g4, 2 + (p & 1));
                bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, g0, p < 2 ? y : x, p < 2 ? x : y);
            } else
                bits = quad_read(c, a[k], g4, p);
            av[k] = mixed ? spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, bits) : half_pair(c, bits);
        }
        for (u32 j = 0; j < 2; j++)
            for (k = 0; k < nb; k++) {
                u32 bits;
                if (!pairs)
                    bits =
                        warp_read(c, b[k], spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, t8, cu(c, 4 * j + p)));
                else if (p < 2)
                    bits = mine[j][k][p] =
                        warp_read(c, b[k], spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, t8h, cu(c, 4 * j + p)));
                else
                    bits = warp_xor(c, mine[j][k][p - 2], 4);
                bv[j][k] = mixed ? spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, bits) : half_pair(c, bits);
            }
        for (u32 h = 0; h < nb; h++) /* the k halves: A's registers 2h (row g) and 2h + 1 (row g + 8), B's h */
            for (u32 i = 0; i < 2; i++)
                for (u32 j = 0; j < 2; j++)
                    acc[i][j] = mixed ? SPV(&c->e, 6916 /* OpFDot2MixAcc32VALVE */, c->t_f32, av[2 * h + i], bv[j][h],
                                            acc[i][j])
                                      : op2(c, SPV_OP_F_ADD, c->t_f32, acc[i][j],
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
    u32 word = spv_emit_load_nonprivate(&c->e, c->t_u32, mem_ptr(c, 0, address, 0));
    u32 shift = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, address, cu(c, 2)), cu(c, 3));
    return spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                           spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, word, shift), cu(c, 0xffff));
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
    u32 lane = warp_lane(c), g = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, lane, cu(c, 2));
    u32 t = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3)), out[4];
    for (u32 m = 0; m < (1u << count); m++) {
        if (!trans) {
            u32 row = warp_read(c, addr, spv_emit_iadd_u32(&c->e, c->t_u32, g, cu(c, 8 * m)));
            u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, row,
                                       spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 2)));
            out[m] = spv_emit_load_nonprivate(&c->e, c->t_u32, mem_ptr(c, 0, at, 0));
        } else {
            u32 r0 = spv_emit_iadd_u32(&c->e, c->t_u32,
                                       spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 1)),
                                       cu(c, 8 * m));
            u32 col = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, g, cu(c, 1));
            u32 lo = shared_u16(c, spv_emit_iadd_u32(&c->e, c->t_u32, warp_read(c, addr, r0), col));
            u32 hi = shared_u16(
                c, spv_emit_iadd_u32(&c->e, c->t_u32,
                                     warp_read(c, addr, spv_emit_iadd_u32(&c->e, c->t_u32, r0, cu(c, 1))), col));
            out[m] = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, lo,
                                     spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, hi, cu(c, 16)));
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
    u32 g = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, lane, cu(c, 2));
    u32 t8 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3)), cu(c, 3));
    u32 from = spv_emit_iadd_u32(&c->e, c->t_u32, t8,
                                 spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, g, cu(c, 1)));
    u32 shift = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, g, cu(c, 1)), cu(c, 4));
    u32 v0 = warp_read(c, src, from), v1 = warp_read(c, src, spv_emit_iadd_u32(&c->e, c->t_u32, from, cu(c, 4)));
    u32 lo = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, v0, shift), cu(c, 0xffff));
    u32 hi = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, v1, shift), cu(c, 0xffff));
    wr(c, in->dst,
       spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, lo,
                       spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, hi, cu(c, 16))));
}
/* the ballot of a predicate over the invocation's 32 lanes, on a subgroup of 32 or 64 */
static u32 ballot32(Ctx* c, u32 pred) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    u32 all = spv_emit_subgroup_ballot(&c->e, c->t_v4u, c->t_u32, pred);
    u32 half = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, system_value(c, 0), cu(c, 5));
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
        v = spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, v, frag_coord(c, 3));
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
            u32 lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, 31));
            u32 eq = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, cu(c, 1), lane);
            u32 lt = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, eq, cu(c, 1));
            u32 le = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, lt, eq);
            return sr == 0x38   ? eq
                   : sr == 0x39 ? lt
                   : sr == 0x3a ? le
                   : sr == 0x3b ? spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, le)
                                : spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, lt);
        }
        case 0x84:
            /* NAK's NAK_SV_VARIABLE_RATE, a fragment's shading rate: its width in pixels in bits 8-15 and its height
               in bits 16-23 (1, 2 or 4), which nak_nir.c reads for load_frag_shading_rate. The runtime shades every
               pixel at the full rate (nvn_cmd.c), so each fragment is 1 by 1. One game's fragment programs take its
               width with PRMT R, R, 0x7771, RZ */
            if (c->stage != ST_FS) {
                fail(c, "S2R system register 0x84 outside a fragment program");
                return c->zero;
            }
            return cu(c, 1u << 8 | 1u << 16);
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
    return sr == 0 ? spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, v, cu(c, 31)) : v;
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
    u32 more = spv_emit_binary(e, SPV_OP_U_LESS_THAN, c->t_bool, i, end);
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
            u32 sum = nocontract(c, spv_emit_binary(e, SPV_OP_F_ADD, c->t_f32, v[2], v[3]));
            v[2] = spv_emit_ternary(e, SPV_OP_SELECT, c->t_f32, on,
                                    nocontract(c, spv_emit_binary(e, SPV_OP_F_MUL, c->t_f32, sum, cf(c, 0.5f))), v[2]);
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
    u32 first = spv_emit_binary(e, SPV_OP_I_EQUAL, c->t_bool, invocation, c->zero);
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
        /* within the device's limit: a count past it is undefined behavior, which can hang the GPU */
        if (g_shader_mesh_tasks_max)
            count = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, count, cu(c, g_shader_mesh_tasks_max));
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
        u32 group = spv_emit_binary(e, SPV_OP_I_MUL, c->t_u32,
                                    spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, v, cu(c, 5)),
                                    cu(c, 32 * c->vertex_words));
        u32 base =
            spv_emit_iadd_u32(e, c->t_u32, group, spv_emit_binary(e, SPV_OP_BITWISE_AND, c->t_u32, v, cu(c, 31)));
        mesh_outputs(c, c->sph, base, v, m->max_vertices, 0, c->out_attr);
        loop_end(c, &loop, v);
    }
    /* the primitives */
    u32 p = loop_begin(c, &loop, invocation, primitives);
    u32 pointer = spv_emit_type_pointer(e, SPV_STORAGE_OUTPUT, c->t_v3u);
    u32 indices = spv_emit_interface_variable(e, SPV_STORAGE_OUTPUT, c->t_v3u, c->t_u32, m->max_primitives,
                                              5296 /* PrimitiveTriangleIndicesEXT */, 0, 0);
    add_interface(c, indices);
    u32 at = spv_emit_iadd_u32(e, c->t_u32, spv_emit_binary(e, SPV_OP_I_MUL, c->t_u32, p, cu(c, 3)), cu(c, 4)),
        index[3];
    for (u32 k = 0; k < 3; k++)
        index[k] =
            isbe_load(c, map, SPV_STORAGE_WORKGROUP, c->isbe_map_words, spv_emit_iadd_u32(e, c->t_u32, at, cu(c, k)));
    /* an index at or past the vertex count reads a vertex the program never wrote: some drivers hang on it (the
       Xid 109). Clamp to the last vertex, as the vertex count itself isn't written */
    u32 last_vertex = cu(c, m->max_vertices - 1);
    for (u32 k = 0; k < 3; k++)
        index[k] = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, index[k], last_vertex);
    spv_emit_store(e, SPV(e, SPV_OP_ACCESS_CHAIN, pointer, indices, p),
                   SPV(e, SPV_OP_COMPOSITE_CONSTRUCT, c->t_v3u, index[0], index[1], index[2]));
    if (c->primitive_words) {
        u32 groups = (m->max_vertices + 31) / 32;
        u32 group = spv_emit_binary(e, SPV_OP_I_MUL, c->t_u32,
                                    spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, p, cu(c, 5)),
                                    cu(c, 32 * c->primitive_words));
        u32 base =
            spv_emit_iadd_u32(e, c->t_u32, group, spv_emit_binary(e, SPV_OP_BITWISE_AND, c->t_u32, p, cu(c, 31)));
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
    pwr(c, p, spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, c->zero, high), uniform);
    pwr(c, q, spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, cu(c, 1), high), uniform);
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
            u32 lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, 3));
            u32 shift = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, cu(c, 6),
                                        spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, lane, cu(c, 1)));
            u32 mode = spv_emit_binary(
                &c->e, SPV_OP_BITWISE_AND, c->t_u32,
                spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, cu(c, swizzle), shift), cu(c, 3));
            u32 x = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                     spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, mode, cu(c, 1)),
                                     spv_emit_unary(&c->e, SPV_OP_F_NEGATE, c->t_f32, a), a);
            u32 y = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                     spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, mode, cu(c, 2)),
                                     spv_emit_unary(&c->e, SPV_OP_F_NEGATE, c->t_f32, b), b);
            u32 sum = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_f32, x, y));
            dwr(c, in,
                to_u(c, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                         spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, mode, cu(c, 3)), b, sum)));
            return;
        }
        case SM86_FMNMX: {
            u32 min = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            int on = (int)sm86_field(in, 80, 1);
            u32 x = ftz(c, src_f(c, &in->a), on), y = ftz(c, src_f(c, &in->b), on);
            u32 lo = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN, x, y);
            u32 hi = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, y);
            dwr(c, in, to_u(c, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, min, lo, hi)));
            return;
        }
        case SM86_FSET: {
            int on = (int)sm86_field(in, 80, 1);
            u32 cmp = spv_emit_fcompare(&c->e, c->t_bool, sm86_field(in, 76, 4), ftz(c, src_f(c, &in->a), on),
                                        ftz(c, src_f(c, &in->b), on));
            u32 acc = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            cmp = combine(c, sm86_field(in, 74, 2), cmp, acc);
            dwr(c, in, to_u(c, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, cmp, cf(c, 1.0f), cf(c, 0.0f))));
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
                /* no negation: bit 72, a's negation in the decoder's fields, is .EX, and 73 the signedness (NAK's
                   OpISetP, whose sources take no modifiers). As a's negation, .EX compared -a: one game's
                   ISETP.NE.AND.EX of a pointer's high word, R-ff (RZ) its a, didn't show it */
                cmp = int_compare(c, sm86_field(in, 76, 3), (int)sm86_field(in, 73, 1), src_bits(c, &in->a),
                                  src_bits(c, &in->b));
                if (sm86_field(in, 72, 1)) { /* .EX: equal high words take the low words' comparison */
                    u32 low = prd(c, sm86_field(in, 68, 3), sm86_field(in, 71, 1), uniform);
                    u32 equal =
                        spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, src_bits(c, &in->a), src_bits(c, &in->b));
                    u32 cmp_op = sm86_field(in, 76, 3);
                    if (cmp_op != 0 && cmp_op != 7)
                        cmp = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_bool, equal, low, cmp);
                }
            }
            u32 acc = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 set_op = sm86_field(in, 74, 2);
            pwr(c, sm86_field(in, 81, 3), combine(c, set_op, cmp, acc), uniform);
            pwr(c, sm86_field(in, 84, 3),
                combine(c, set_op, spv_emit_unary(&c->e, SPV_OP_LOGICAL_NOT, c->t_bool, cmp), acc), uniform);
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
            dwr(c, in, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, p, x, y));
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
                if (in->b.neg) x = spv_emit_unary(&c->e, SPV_OP_F_NEGATE, c->t_f32, x);
            } else
                x = src_f(c, &in->b);
            switch (fn) {
                case 0:
                case 1: /* the argument in turns, as the hardware takes it */
                    v = spv_emit_glsl_unary(
                        &c->e, c->t_f32, c->glsl, fn ? SPV_GLSL_SIN : SPV_GLSL_COS,
                        spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, x, cf(c, 6.28318530717958647692f)));
                    break;
                case 2: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_EXP2, x); break;
                case 3: v = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_LOG2, x); break;
                case 4: v = spv_emit_binary(&c->e, SPV_OP_F_DIV, c->t_f32, cf(c, 1.0f), x); break;
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
                if (in->b.neg) from = spv_emit_unary(&c->e, SPV_OP_F_NEGATE, c->t_f32, from);
            } else
                from = src_f(c, &in->b);
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            u32 x = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[sm86_field(in, 78, 2)], from);
            int is_signed = (int)sm86_field(in, 72, 1);
            /* the hardware saturates, and a NaN gives 0 */
            u32 nan = spv_emit_unary(&c->e, SPV_OP_IS_NAN, c->t_bool, x), v;
            if (dsize < 2) {
                /* an 8- or 16-bit result saturates to its range, then fills the register sign or zero extended */
                u32 bits = 8u << dsize;
                float lo = is_signed ? -(float)(1u << (bits - 1)) : 0.0f;
                float hi = is_signed ? (float)((1u << (bits - 1)) - 1) : (float)((1u << bits) - 1);
                u32 clamped = spv_emit_glsl_binary(
                    &c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN,
                    spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, lo)), cf(c, hi));
                v = is_signed ? to_u(c, spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32, clamped))
                              : spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_U, c->t_u32, clamped);
            } else if (is_signed) {
                u32 high = spv_emit_fcompare(&c->e, c->t_bool, 6, x, cf(c, 2147483648.0f));
                u32 clamped = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, -2147483648.0f));
                v = to_u(c, spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32, clamped));
                v = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, high, cu(c, 0x7fffffffu), v);
            } else {
                u32 high = spv_emit_fcompare(&c->e, c->t_bool, 6, x, cf(c, 4294967296.0f));
                u32 clamped = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, x, cf(c, 0.0f));
                v = spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_U, c->t_u32, clamped);
                v = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, high, cu(c, 0xffffffffu), v);
            }
            dwr(c, in, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, nan, c->zero, v));
            return;
        }
        case SM86_F2IP: {
            /* F2IP.U8.F32.NTZ d, a, b, c: the floats a and b each rounded to an integer and saturated to 8 bits, a
               NaN 0 (.NTZ), packed as PTX's cvt.pack packs I2IP's: d = c << 16 | a' << 8 | b'. Bits 72-79
               (CuAssembler's sm_86 records): 0x04 .U8.F32.NTZ, bit 76 .S8 (-128 to 127), bit 75 .RELU (0 to 127).
               Bits 78-79 are the rounding, in F2I's place and order (nearest even, down, up, toward zero):
               CuAssembler records none but the nearest, and one game's mesh programs give 3 to F2IP.U8 of a float
               times 255, as a shader's uint() truncates. Its compute programs pack colours with F2IP.U8 then PRMT */
            u32 flags = sm86_field(in, 72, 8);
            if ((flags & ~0xd8u) != 0x04) {
                fail(c, "F2IP with flags 0x%x", flags);
                return;
            }
            static const u16 rounding[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            int s8 = (flags >> 4) & 1, relu = (flags >> 3) & 1;
            float lo = s8 && !relu ? -128.0f : 0.0f, hi = s8 ? 127.0f : 255.0f;
            u32 part[2];
            for (u32 k = 0; k < 2; k++) {
                u32 x = to_f(c, rd(c, sm86_field(in, k ? 32 : 24, 8)));
                u32 nan = spv_emit_unary(&c->e, SPV_OP_IS_NAN, c->t_bool, x);
                u32 r = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, rounding[flags >> 6], x);
                r = spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MIN,
                                         spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_N_MAX, r, cf(c, lo)),
                                         cf(c, hi));
                r = s8 ? to_u(c, spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_S, c->t_i32, r))
                       : spv_emit_unary(&c->e, SPV_OP_CONVERT_F_TO_U, c->t_u32, r);
                r = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, r, cu(c, 0xff));
                part[k] = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, nan, c->zero, r);
            }
            u32 v = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                    spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, part[0], cu(c, 8)),
                                    part[1]);
            v = spv_emit_binary(
                &c->e, SPV_OP_BITWISE_OR, c->t_u32,
                spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, rd(c, sm86_field(in, 64, 8)), cu(c, 16)),
                v);
            wr(c, in->dst, v);
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
                x = is_signed ? to_u(c, spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                                                         spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, x), at,
                                                         cu(c, width)))
                              : spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, x, at, cu(c, width));
            }
            u32 type = mode ? f64_type(c) : c->t_f32, v;
            if (!mode && src_size == 2 && (g_shader_i2f_nearest & (is_signed ? 1 : 2)))
                v = spv_emit_int_to_f32_nearest(&c->e, x, is_signed);
            else
                v = is_signed ? spv_emit_unary(&c->e, SPV_OP_CONVERT_S_TO_F, type,
                                               spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, x))
                              : spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, type, x);
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
            u32 v = spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32, src_int(c, &in->a), src_int(c, &in->b));
            v = spv_emit_iadd_u32(&c->e, c->t_u32, v, src_int(c, &in->c));
            if (sm86_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
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
                u32 xi = spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, x),
                    yi = spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, y);
                v = to_u(c, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_i32, min,
                                             spv_emit_glsl_binary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_MIN, xi, yi),
                                             spv_emit_glsl_binary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_MAX, xi, yi)));
            } else
                v = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, min,
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
            if (sm86_field(in, 63, 1)) x = spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, x);
            u32 bit = sm86_field(in, 73, 1) ? to_u(c, SPV_EXT(&c->e, c->t_i32, 74 /* FindSMsb */,
                                                              spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, x)))
                                            : SPV_EXT(&c->e, c->t_u32, 75 /* FindUMsb */, x);
            if (sm86_field(in, 74, 1))
                bit = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                                       spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, bit, cu(c, 0xffffffffu)), bit,
                                       spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, cu(c, 31), bit));
            dwr(c, in, bit);
            return;
        }
        case SM86_POPC:
        case SM86_UPOPC: {
            u32 x = src_bits(c, &in->b);
            if (sm86_field(in, 63, 1)) x = spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, x);
            dwr(c, in, spv_emit_unary(&c->e, SPV_OP_BIT_COUNT, c->t_u32, x));
            return;
        }
        case SM86_BREV:
        case SM86_UBREV: dwr(c, in, spv_emit_unary(&c->e, SPV_OP_BIT_REVERSE, c->t_u32, src_bits(c, &in->b))); return;
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
                    bits = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, bits,
                                           spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, prd(c, i, 0, uniform),
                                                            cu(c, 1u << i), c->zero));
            pr = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, bits, cu(c, mask & 0xff));
            u32 keep = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, src_bits(c, &in->a),
                                       cu(c, ~((mask & 0xffu) << (8 * byte))));
            dwr(c, in,
                spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, keep,
                                spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, pr, cu(c, 8 * byte))));
            return;
        }
        case SM86_F2FP: {
            /* F2FP.PACK_AB Rd, Ra, Rb: a in the high half, b in the low one, as PTX's cvt.rn.f16x2.f32; bit 76
               BF16 (CuAssembler sm_86). The halves round to nearest */
            if (sm86_field(in, 78, 1)) {
                /* .MERGE_C (NAK's note): b converted in the low half, c's low half (its high one with .H1, bit 72) in
                   the high half, a unused */
                u32 keep = src_bits(c, &in->c);
                if (sm86_field(in, 72, 1))
                    keep = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, keep, cu(c, 16));
                u32 low = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16,
                                  spv_emit_vec2_construct(&c->e, c->t_v2f, src_f(c, &in->b), cf(c, 0.0f)));
                dwr(c, in,
                    spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                    spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, keep, cu(c, 16)), low));
                return;
            }
            u32 hi = src_f(c, &in->a), lo = src_f(c, second(in));
            u32 v;
            if (sm86_field(in, 76, 1))
                v = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                    spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, bf16(c, hi), cu(c, 16)),
                                    bf16(c, lo));
            else
                v = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, spv_emit_vec2_construct(&c->e, c->t_v2f, lo, hi));
            dwr(c, in, v);
            return;
        }
        case SM86_IABS:
            dwr(c, in,
                to_u(c, spv_emit_glsl_unary(&c->e, c->t_i32, c->glsl, SPV_GLSL_S_ABS,
                                            spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, src_bits(c, &in->b)))));
            return;
        case SM86_ULEA:
        case SM86_LEA: {
            u32 shift = sm86_field(in, 75, 5), p = sm86_field(in, 81, 3);
            int high = (int)sm86_field(in, 80, 1), x = (int)sm86_field(in, 74, 1);
            u32 a = src_bits(c, &in->a), shifted;
            if (high) /* the high word of (c:a) << shift */
                shifted = shift ? spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                                  spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                                                  src_bits(c, &in->c), cu(c, shift)),
                                                  spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, a,
                                                                  cu(c, 32 - shift)))
                                : src_bits(c, &in->c);
            else
                shifted = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, a, cu(c, shift));
            if (sm86_field(in, 72, 1)) shifted = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, c->zero, shifted);
            if (p >= SM86_PT && !x) {
                dwr(c, in, spv_emit_iadd_u32(&c->e, c->t_u32, shifted, src_int(c, &in->b)));
                return;
            }
            u32 sum = op2(c, SPV_OP_I_ADD, u64_type(c), to64(c, shifted), to64(c, src_int(c, &in->b)));
            if (x) sum = op2(c, SPV_OP_I_ADD, u64_type(c), sum, carry_in(c, 87, in, uniform));
            dwr(c, in, lo32(c, sum));
            pwr(c, p, spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, c->zero, hi32(c, sum)), uniform);
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
            pwr(c, sm86_field(in, 81, 3), spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool, v, c->zero), uniform);
            return;
        }
        case SM86_USHF:
        case SM86_SHF: {
            u32 dtype = sm86_field(in, 73, 2), bits = dtype < 2 ? 64 : 32;
            int right = (int)sm86_field(in, 76, 1), wrap = (int)sm86_field(in, 75, 1),
                high = (int)sm86_field(in, 80, 1);
            int is_signed = dtype == 0 || dtype == 2;
            u32 s = src_bits(c, &in->b);
            s = wrap ? spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, s, cu(c, bits - 1))
                     : spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, s, cu(c, bits));
            u32 value = pair64(c, src_bits(c, &in->a), src_bits(c, &in->c));
            /* a shift by 64 is out of SPIR-V's range: split it in two */
            u32 s64 = to64(c, s), half = c64(c, 0);
            u32 big = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, cu(c, 63), s);
            u32 s_part = spv_emit_ternary(&c->e, SPV_OP_SELECT, u64_type(c), big, c64(c, 63), s64);
            u32 extra = spv_emit_ternary(&c->e, SPV_OP_SELECT, u64_type(c), big, c64(c, 1), half);
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
                        spv_emit_binary(
                            &c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
                            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, x, cu(c, 1u << (i + 8 * byte))),
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
               40-52 (an immediate) or 64 (a register: SHFL (imm, reg) 0x989 and SHFL (reg, reg) 0x389, as
               CuAssembler's sm_86 records "SHFL.UP PT, R7, R8, 0x1, RZ" and "SHFL.DOWN P1, R32, R33, R32, R31"
               show). One game's prefix sums are SHFL.UP P0, R12, R19, 0x1, RZ */
            u32 mode = sm86_field(in, 58, 2), cc = sm86_field(in, 40, 13), seg, not_seg = 0, clamp_bits;
            int c_reg = op == SM86_SHFL_IR || op == SM86_SHFL_RR;
            u32 creg = c_reg ? rd(c, sm86_field(in, 64, 8)) : 0;
            u32 b =
                op == SM86_SHFL_II || op == SM86_SHFL_IR ? cu(c, sm86_field(in, 53, 5)) : rd(c, sm86_field(in, 32, 8));
            u32 id = system_value(c, 0), lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, id, cu(c, 31));
            if (c_reg) { /* c's fields as the immediate's, at run time */
                seg = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                                      spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, creg, cu(c, 8)),
                                      cu(c, 31));
                not_seg = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                                          spv_emit_unary(&c->e, SPV_OP_NOT, c->t_u32, seg), cu(c, 31));
            } else
                seg = cu(c, (cc >> 8) & 31);
            u32 min_lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, seg);
            clamp_bits = c_reg ? spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, creg, not_seg)
                               : cu(c, cc & ~(cc >> 8) & 31);
            u32 max_lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, min_lane, clamp_bits);
            u32 j, ok;
            if (mode == 0) {
                if (!c_reg) not_seg = cu(c, ~(cc >> 8) & 31);
                j = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, min_lane,
                                    spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, b, not_seg));
                ok = op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, j, max_lane);
            } else if (mode == 1) {
                j = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32, lane, b);
                ok = op2(c, SPV_OP_S_GREATER_THAN_EQUAL, c->t_bool, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, j),
                         spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, min_lane));
            } else {
                j = mode == 2 ? spv_emit_iadd_u32(&c->e, c->t_u32, lane, b)
                              : spv_emit_binary(&c->e, SPV_OP_BITWISE_XOR, c->t_u32, lane, b);
                ok = op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, j, max_lane);
            }
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
            /* the lane in the invocation's own 32, on a subgroup of 32 or 64 */
            u32 source = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32,
                                         spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, id, cu(c, ~31u)),
                                         spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, j, cu(c, 31)));
            u32 value = rd(c, sm86_field(in, 24, 8));
            u32 got = SPV(&c->e, 345 /* OpGroupNonUniformShuffle */, c->t_u32, cu(c, 3), value, source);
            wr(c, in->dst, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, ok, got, value));
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
            u32 id = system_value(c, 0), base = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, id, cu(c, ~31u));
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
            spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
            u32 mask = c->zero;
            for (u32 j = 0; j < 32; j++) {
                u32 lane = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, base, cu(c, j));
                u32 same = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool,
                                           SPV(&c->e, 345, c->t_u32, cu(c, 3), lo, lane), lo);
                if (pair)
                    same = op2(c, SPV_OP_LOGICAL_AND, c->t_bool, same,
                               spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool,
                                               SPV(&c->e, 345, c->t_u32, cu(c, 3), hi, lane), hi));
                mask = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, mask,
                                       spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, same, cu(c, 1u << j), c->zero));
            }
            u32 active = ballot32(c, c->true_v);
            mask = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, mask, active);
            if (any)
                wr(c, in->dst, mask);
            else {
                u32 all = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, mask, active);
                wr(c, in->dst, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, all, mask, c->zero));
                pwr(c, sm86_field(in, 81, 3), all, 0);
            }
            return;
        }
        case SM86_ATOMG:
        case SM86_ATOMG_NOU:
        case SM86_RED: {
            /* NAK's OpAtom on global memory: atomic_opcode's operation at 87-90, the data at 32; RED returns nothing.
               Of type 3 (F32, .FTZ.RN) the ADD alone, atomic_fadd: one game's compute programs sum positions into a
               buffer with RED.E.ADD.F32.FTZ.RN */
            u32 how = sm86_field(in, 87, op == SM86_RED ? 3 : 4), type = sm86_field(in, 73, 3);
            u32 opcode = atomic_opcode(type, how);
            int fadd = type == 3 && how == 0;
            if (!opcode && !fadd) {
                fail(c, "%s of type %u, operation %u", sm86_ops[op].name, type, how);
                return;
            }
            if (!c->addr_key[c->cur] && c->addr_key[2 * (c->n + 1) + c->cur]) {
                global_dynamic(c, in, DYN_ATOMIC, opcode, fadd);
                return;
            }
            u32 address, buffer = global_address(c, in, op == SM86_ATOMG || (op == SM86_RED && sm86_field(in, 91, 1)),
                                                 1, &address);
            if (!buffer) return;
            mark_read(c, c->gkey);
            mark_written(c, c->gkey, address, 4);
            u32 old = fadd ? atomic_fadd(c, buffer, address, rd(c, sm86_field(in, 32, 8)))
                           : spv_emit_atomic_u32(&c->e, c->t_u32, opcode, mem_ptr(c, buffer, address, 0),
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
                a64 = spv_emit_unary(
                    &c->e, SPV_OP_BITCAST, t64,
                    SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, a32)));
                b64 = spv_emit_unary(
                    &c->e, SPV_OP_BITCAST, t64,
                    SPV(&c->e, SPV_OP_S_CONVERT, i64, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, b32)));
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
                product = op2(c, SPV_OP_I_MUL, t64,
                              spv_emit_unary(&c->e, SPV_OP_BITCAST, t64,
                                             SPV(&c->e, SPV_OP_S_CONVERT, i64,
                                                 spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, a32))),
                              spv_emit_unary(&c->e, SPV_OP_BITCAST, t64,
                                             SPV(&c->e, SPV_OP_S_CONVERT, i64,
                                                 spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, b32))));
            } else
                product = op2(c, SPV_OP_I_MUL, t64, to64(c, a32), to64(c, b32));
            u32 v = spv_emit_iadd_u32(&c->e, c->t_u32, hi32(c, product), src_int(c, &in->c));
            if (sm86_field(in, 74, 1))
                v = spv_emit_iadd_u32(&c->e, c->t_u32, v,
                                      spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                                                       prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0),
                                                       cu(c, 1), c->zero));
            dwr(c, in, v);
            return;
        }
        case SM86_SGXT:
        case SM86_USGXT: {
            /* the low b bits (at most 32) of a, sign (bit 73) or zero extended; 0 bits give 0 */
            u32 bits = spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, src_bits(c, &in->b), cu(c, 32));
            u32 a = src_bits(c, &in->a);
            u32 v = sm86_field(in, 73, 1)
                        ? to_u(c, spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                                                   spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, a), c->zero, bits))
                        : spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, a, c->zero, bits);
            dwr(c, in,
                spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                                 spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, bits, c->zero), c->zero, v));
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
                u32 nib = spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, sel, cu(c, 4 * k), cu(c, 4));
                u32 from_c =
                    spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
                                    spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, nib, cu(c, 4)), c->zero);
                u32 word = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, from_c, hi, a);
                u32 shift =
                    spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                    spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, nib, cu(c, 3)), cu(c, 3));
                u32 byte = spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, word, shift, cu(c, 8));
                u32 spread =
                    spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
                                    spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, nib, cu(c, 8)), c->zero);
                u32 sign = spv_emit_ternary(
                    &c->e, SPV_OP_SELECT, c->t_u32,
                    spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
                                    spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, byte, cu(c, 0x80)), c->zero),
                    cu(c, 0xff), c->zero);
                byte = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, spread, sign, byte);
                v = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, v,
                                    spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, byte, cu(c, 8 * k)));
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
            u32 r = nocontract(c, op2(c, SPV_OP_F_ADD, c->t_v2f,
                                      nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_v2f, v[0], v[1])), v[2]));
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
                    lane[k] = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32,
                                               op2(c, SPV_OP_LOGICAL_AND, c->t_bool, holds, below), cf(c, 0.0f), x);
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
                r = nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_v2f, v[0], v[1]));
                if (sm86_field(in, 77, 1))
                    r = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_CLAMP, r,
                                spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 0.0f), cf(c, 0.0f)),
                                spv_emit_vec2_construct(&c->e, c->t_v2f, cf(c, 1.0f), cf(c, 1.0f)));
            } else {
                u32 min = prd(c, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0), lane[2];
                u32 lo = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_MIN, v[0], v[1]);
                u32 hi = SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_N_MAX, v[0], v[1]);
                for (u32 k = 0; k < 2; k++)
                    lane[k] = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, min,
                                               spv_emit_vec4_extract(&c->e, c->t_f32, lo, k),
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
            /* ULDC UR, c[bank][UR + offset]: the uniform register at 24, the offset 38-53 signed. Of NAK's MemType
               (73-75) as LDC's: a byte or a halfword (0 to 3) as cbuf_subword reads it, a word, or a pair. One game's
               compute program walks a byte code in a constant buffer with ULDC.U8 UR, c[0x4][UR15 + k] */
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            if (type > 5) {
                fail(c, "ULDC (indexed) of type %u", type);
                return;
            }
            u32 bias = off & 0x8000u ? off | 0xffff0000u : off, d = sm86_field(in, 16, 6);
            u32 index = urd(c, sm86_field(in, 24, 6));
            if (type < 4) {
                uwr(c, d, cbuf_subword(c, bank, bias, index, type));
                return;
            }
            uwr(c, d, cbuf_load(c, bank, bias, index));
            if (type == 5 && d < SM86_URZ) uwr(c, d + 1, cbuf_load(c, bank, bias + 4, index));
            return;
        }
        case SM86_ULDC: {
            /* the types as ULDC.IDX's: CuAssembler's sm_86 records give ULDC.U8, .S8 and .U16 too */
            u32 type = sm86_field(in, 73, 3), bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16);
            if (sm86_field(in, 91, 1) || type > 5) {
                fail(c, "ULDC of type %u", type);
                return;
            }
            u32 d = sm86_field(in, 16, 6);
            if (type < 4) {
                uwr(c, d, cbuf_subword(c, bank, off, 0, type));
                return;
            }
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
                u32 at = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32,
                                         spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, off)), cu(c, h->first));
                spv_emit_store(&c->e, h->var, op2(c, SPV_OP_U_DIV, c->t_u32, at, cu(c, h->stride)));
            }
            if (type < 4) {
                wr(c, in->dst, cbuf_subword(c, bank, off, index, type));
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
            /* a pointer from memory, or derived from one, that no buffer key names */
            if (!c->addr_key[c->cur] && c->addr_key[2 * (c->n + 1) + c->cur]) {
                global_dynamic(c, in, load ? DYN_LOAD : DYN_STORE, 0, 0);
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
                v = to_f(c, spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                                             spv_emit_load(&c->e, c->t_bool, c->front_facing), cu(c, 0xffffffffu),
                                             c->zero));
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
                        v = spv_emit_ternary(
                            &c->e, SPV_OP_SELECT, c->t_u32,
                            spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, at, cu(c, 0x80 + 16 * i + 4 * k)),
                            to_u(c, ipa_input(c, i, k, loc, freq)), v);
            wr(c, in->dst, v);
            return;
        }
        case SM86_TEX_U:
        case SM86_TLD_U:
        case SM86_TLD4_U:
        case SM86_TXQ_U:
            if (!c->handle || !c->handle[c->cur].slot) { /* memory_handles found no slot */
                uniform_handle(c, in);
                return;
            }
            if (op == SM86_TLD_U)
                texel_fetch(c, in);
            else if (op == SM86_TXQ_U)
                texture_query(c, in);
            else
                texture(c, in);
            return;
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
            c->si->has_discard = 1; /* the renderer tests depth before the program only for one that keeps its pixels */
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
    u32 set = spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool, t,
                              c->zero); /* before the merge, which precedes the branch */
    spv_emit_selection_merge(&c->e, go);
    spv_emit_branch_conditional(&c->e, set, pending, go);
    spv_emit_label(&c->e, pending);
    if (!se->st->scope[parent].loop) {
        if (s->arrive && s->beyond)
            spv_emit_store(&c->e, se->flag,
                           spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                                            spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, t, here), c->zero, t));
        else if (s->arrive)
            spv_emit_store(&c->e, se->flag, c->zero);
        se_break(c, se, parent);
    } else if (s->arrive && s->beyond) {
        u32 again = spv_emit_id(&c->e), out = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
        u32 arrived = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, t, here);
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
        int never = sm86_never(in);
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
        } else if ((in->op == SM86_EXIT && sm86_always(in)) || in->op == SM86_RET)
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
            int always = sm86_always(in), never = sm86_never(in);
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
    /* a BRA keeps its predicate operand (sm86_has_cond), a CALL's or a RET's is PT; no barriers */
    u64 cond = in->op == SM86_BRA ? sm86_field(in, 87, 4) : 7;
    u64 hi = field >> 30 | cond << 23 | 7ull << 46 | 7ull << 49;
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
        while (end < n &&
               !(out[end].op == SM86_BRA && sm86_always(&out[end]) && sm86_branch_target(&out[end], end) == end))
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
/* ---- texture handles from memory ---- */

/* An Ounce program may load a texture's handle from memory: one game's material programs read it from a table of
   materials in a storage buffer (LDG.64 [R + UR], UR a ULDC.64 of cbuf 0), at a record that the draw's constant
   buffers pick (c[4][0x88] & 0xfffff plus c[5][0x2ec], times 32, plus 0x18), then R2UR it into the uniform register
   pair of a TEX whose handle is in a uniform register (the texture's word, then its >> 20, the sampler's index), or
   into a TEX.B's handle register. Where every value the address takes is the same for the whole draw (constant
   buffer words, immediates, and words loaded from such addresses), the translation keeps the instructions that make
   the handle (ShaderInfo.memh) and the slot names the handle's value (SNX_TEX_MEMORY, SH_PROBE): the renderer runs
   them on the draw's constant buffers and memory (sm86_eval) and binds what they give as a constant buffer's handle.
   The program still runs them itself, for what else it does with the words (a test of the handle against 0) */
enum { MH_R, MH_UR, MH_P, MH_UP }; /* the files of a value: registers, uniform registers, predicates, uniform ones */
typedef struct MhDep {
    u32 at, file, reg, writer; /* instruction at reads the value, which writer wrote */
} MhDep;
/* the words an instruction that sm86_eval runs writes from its destination: a pair's two, a load's by its type */
static u32 mh_words(const Sm86Insn* in) {
    u32 type = sm86_field(in, 73, 3);
    switch (in->op) {
        case SM86_LDG:
        case SM86_LDG_NOU: return type == 5 ? 2 : type == 6 ? 4 : 1;
        case SM86_LDC:
        case SM86_ULDC:
        case SM86_ULDC_IDX: return type == 5 ? 2 : 1;
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE: return 2;
        case SM86_CS2R: return sm86_field(in, 80, 1) ? 2 : 1;
        case SM86_ISETP:
        case SM86_UISETP:
        case SM86_PLOP3:
        case SM86_UPLOP3: return 0;
        default: return 1;
    }
}
static int mh_uniform_dst(const Sm86Insn* in) {
    return sm86_ops[in->op].kind == SM86_K_UALU || in->op == SM86_ULDC || in->op == SM86_ULDC_IDX ||
           in->op == SM86_R2UR;
}
/* whether an instruction may write value r of the file: exactly for those sm86_eval runs, else as far as the
   translation knows (writes_word, writes_uniform, and the fields of the instructions that write predicates) */
static int mh_may_write(const Sm86Insn* in, int file, u32 r) {
    int uk = sm86_ops[in->op].kind == SM86_K_UALU;
    if (file == MH_P || file == MH_UP) {
        int u = file == MH_UP, p81 = sm86_field(in, 81, 3) == r, p84 = sm86_field(in, 84, 3) == r;
        switch (in->op) {
            case SM86_IADD3:
            case SM86_UIADD3:
            case SM86_ISETP:
            case SM86_UISETP:
            case SM86_FSETP:
            case SM86_DSETP:
            case SM86_HSETP2: return uk == u && (p81 || p84);
            case SM86_PLOP3: return !u && (p81 || p84);
            case SM86_UPLOP3: return u && (p81 || p84);
            case SM86_LEA:
            case SM86_ULEA:
            case SM86_LOP3:
            case SM86_ULOP3: return uk == u && p81;
            case SM86_UVOTE: return u && p81;
            case SM86_R2P: return !u;
            case SM86_VOTE:
            case SM86_MATCH:
            case SM86_SHFL_II:
            case SM86_SHFL_RI:
            case SM86_SHFL_IR:
            case SM86_SHFL_RR:
            /* the fault predicates of the texture and surface loads */
            case SM86_TEX:
            case SM86_TEX_B:
            case SM86_TLD:
            case SM86_TLD_B:
            case SM86_TLD4:
            case SM86_TLD4_B:
            case SM86_TEX_U:
            case SM86_TLD_U:
            case SM86_TLD4_U:
            case SM86_SULD_P:
            case SM86_SULD_D:
            case SM86_SULD_P_B:
            case SM86_SULD_D_B: return !u && p81;
            default: return 0;
        }
    }
    if (sm86_eval_takes(in)) {
        int u = mh_uniform_dst(in);
        u32 d = u && sm86_ops[in->op].kind != SM86_K_UALU ? sm86_field(in, 16, 6) : in->dst;
        return u == (file == MH_UR) && d < (u ? SM86_URZ : SM86_RZ) && r >= d && r < d + mh_words(in);
    }
    if (file == MH_UR) {
        for (u32 k = 0; k < 4 && k <= r; k++)
            if (writes_uniform(in, r - k) && (k == 0 || in->op == SM86_ULDCG)) return 1;
        return 0;
    }
    if (in->op == SM86_IPA || in->op == SM86_S2R) return in->dst == r; /* one word, where may_write takes two */
    return writes_word(in, r);
}
#define MH_FLOW_PREDS(k)                                                                                               \
    do {                                                                                                               \
        if ((k) == 0) ok = 0;                                                                                          \
        if ((k) > 0 && falls_through(&c->in[(k) - 1]) && !seen[(k) - 1]) seen[(k) - 1] = 1, stack[top++] = (k) - 1;    \
        for (u32 j = c->jump_first[k]; j < c->jump_first[(k) + 1]; j++)                                                \
            if (!seen[c->jump_from[j]]) seen[c->jump_from[j]] = 1, stack[top++] = c->jump_from[j];                     \
    } while (0)
/* The one instruction whose write of value r of the file reaches instruction i on every path there, unguarded, as
   reaching_write finds a register's: -1 when that can't be shown, -2 for RZ, URZ and PT, which no one writes */
static int mh_writer(Ctx* c, u32 i, int file, u32 r) {
    if ((file == MH_R && r >= SM86_RZ) || (file == MH_UR && r >= SM86_URZ) || (file >= MH_P && r >= SM86_PT)) return -2;
    if (!flow_sources(c)) return -1;
    u32 n = c->n, *stack = malloc(((size_t)n + 1) * sizeof *stack), top = 0;
    u8* seen = calloc((size_t)n + 1, 1);
    int ok = stack && seen, found = -1;
    if (ok) MH_FLOW_PREDS(i);
    while (ok && top) {
        u32 k = stack[--top];
        const Sm86Insn* in = &c->in[k];
        if (!mh_may_write(in, file, r)) {
            MH_FLOW_PREDS(k);
            continue;
        }
        if (!sm86_always(in) || (found >= 0 && (u32)found != k)) ok = 0;
        found = (int)k;
    }
    free(stack);
    free(seen);
    return ok ? found : -1;
}
#undef MH_FLOW_PREDS
static const char* mh_file_name(int file) {
    return file == MH_R ? "R" : file == MH_UR ? "UR" : file == MH_P ? "P" : "UP";
}
/* why a handle's instructions can't be found, which memory_handles takes, the program going on */
static int mh_fail(Ctx* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->mh_why, sizeof c->mh_why, fmt, ap);
    va_end(ap);
    return 0;
}
static int mh_add(Ctx* c, u32 k, u32 reader);
/* instruction at, of ShaderInfo.memh or the texture instruction, reads value r of the file: its writer joins them */
static int mh_need(Ctx* c, u32 at, int file, u32 r) {
    int k = mh_writer(c, at, file, r);
    if (k == -2) return 1;
    if (k < 0 || (u32)k >= at)
        return mh_fail(c, "a texture handle from memory whose %s%u at %u has no one writer before", mh_file_name(file),
                       r, at);
    if (c->nmh_dep == c->mh_dep_cap) {
        u32 cap = c->mh_dep_cap ? 2 * c->mh_dep_cap : 64;
        MhDep* grown = realloc(c->mh_dep, cap * sizeof *grown);
        if (!grown) {
            fail(c, "out of memory");
            return 0;
        }
        c->mh_dep = grown, c->mh_dep_cap = cap;
    }
    c->mh_dep[c->nmh_dep++] = (MhDep){at, (u32)file, r, (u32)k};
    return mh_add(c, (u32)k, at);
}
static int mh_src(Ctx* c, u32 at, const Sm86Src* s) {
    if (s->kind == SM86_SRC_REG) return mh_need(c, at, MH_R, s->reg);
    if (s->kind == SM86_SRC_UREG) return mh_need(c, at, MH_UR, s->reg);
    if (s->kind == SM86_SRC_CBUF && (s->bindless || s->bank >= 18))
        return mh_fail(c, "a texture handle from memory made with constant buffer %u", s->bank);
    return 1;
}
static int mh_pred(Ctx* c, u32 at, int uniform, u32 field) {
    return mh_need(c, at, uniform ? MH_UP : MH_P, sm86_field(&c->in[at], field, 3));
}
/* instruction k, which reader reads, joins ShaderInfo.memh, with the writers of what it reads */
static int mh_add(Ctx* c, u32 k, u32 reader) {
    if (c->mh_mark[k]) return 1;
    const Sm86Insn* in = &c->in[k];
    if (!sm86_eval_takes(in))
        return mh_fail(c, "a texture handle from memory made with %s at %u (for %u)", sm86_ops[in->op].name, k, reader);
    if (c->nmh >= SH_MEMH_MAX)
        return mh_fail(c, "texture handles from memory made with more than %u instructions", SH_MEMH_MAX);
    c->nmh++;
    c->mh_mark[k] = 1;
    c->mh_new[c->nmh_new++] = k;
    int op = in->op, u = sm86_ops[op].kind == SM86_K_UALU;
    switch (op) {
        case SM86_MOV:
        case SM86_UMOV: return mh_src(c, k, &in->b);
        case SM86_R2UR: return mh_need(c, k, MH_R, sm86_field(in, 24, 8));
        case SM86_CS2R: return 1;
        case SM86_IADD3:
        case SM86_UIADD3:
            return mh_src(c, k, &in->a) && mh_src(c, k, &in->b) && mh_src(c, k, &in->c) &&
                   (!sm86_field(in, 74, 1) || (mh_pred(c, k, u, 87) && mh_pred(c, k, u, 77)));
        case SM86_IMAD:
        case SM86_UIMAD:
        case SM86_LEA:
        case SM86_ULEA:
            return mh_src(c, k, &in->a) && mh_src(c, k, &in->b) &&
                   ((op != SM86_IMAD && op != SM86_UIMAD && !sm86_field(in, 80, 1)) || mh_src(c, k, &in->c)) &&
                   (!sm86_field(in, 74, 1) || mh_pred(c, k, u, 87));
        case SM86_IMAD_WIDE:
        case SM86_UIMAD_WIDE:
            if (!mh_src(c, k, &in->a) || !mh_src(c, k, &in->b) || !mh_src(c, k, &in->c)) return 0;
            if (in->c.kind == SM86_SRC_REG) return mh_need(c, k, MH_R, in->c.reg >= SM86_RZ ? SM86_RZ : in->c.reg + 1);
            if (in->c.kind == SM86_SRC_UREG)
                return mh_need(c, k, MH_UR, in->c.reg >= SM86_URZ ? SM86_URZ : in->c.reg + 1);
            return 1;
        case SM86_LOP3:
        case SM86_ULOP3:
        case SM86_SHF:
        case SM86_USHF:
        case SM86_PRMT:
        case SM86_UPRMT: return mh_src(c, k, &in->a) && mh_src(c, k, &in->b) && mh_src(c, k, &in->c);
        case SM86_SEL:
        case SM86_USEL:
        case SM86_IMNMX:
        case SM86_UIMNMX: return mh_src(c, k, &in->a) && mh_src(c, k, &in->b) && mh_pred(c, k, u, 87);
        case SM86_ISETP:
        case SM86_UISETP:
            return mh_src(c, k, &in->a) && mh_src(c, k, &in->b) && mh_pred(c, k, u, 87) &&
                   (!sm86_field(in, 72, 1) || mh_pred(c, k, u, 68));
        case SM86_PLOP3:
        case SM86_UPLOP3: {
            int up = op == SM86_UPLOP3;
            return mh_pred(c, k, up, 87) && mh_pred(c, k, up, 77) && mh_pred(c, k, up || sm86_field(in, 67, 1), 68);
        }
        case SM86_ULDC:
        case SM86_ULDC_IDX:
        case SM86_LDC:
            if (sm86_field(in, 54, 5) >= 18)
                return mh_fail(c, "a texture handle from memory made with constant buffer %u", sm86_field(in, 54, 5));
            if (op == SM86_ULDC_IDX) return mh_need(c, k, MH_UR, sm86_field(in, 24, 6));
            if (op == SM86_LDC) return mh_need(c, k, MH_R, sm86_field(in, 24, 8));
            return 1;
        case SM86_LDG:
        case SM86_LDG_NOU: {
            int uniform_form = op == SM86_LDG;
            u32 reg = sm86_field(in, 24, 8), ur = uniform_form ? sm86_field(in, 32, 6) : SM86_URZ;
            if (!mh_need(c, k, MH_R, reg)) return 0;
            if (sm86_field(in, uniform_form ? 90 : 72, 1) && reg < SM86_RZ && !mh_need(c, k, MH_R, reg + 1)) return 0;
            if (ur < SM86_URZ && !mh_need(c, k, MH_UR, ur)) return 0;
            if (ur < SM86_URZ && sm86_field(in, 72, 1) && !mh_need(c, k, MH_UR, ur + 1)) return 0;
            return 1;
        }
        default: return mh_fail(c, "a texture handle from memory made with %s", sm86_ops[op].name);
    }
}
/* the probe (SH_PROBE, by instruction index until mh_finish) of value r of the file at texture instruction at, whose
   writer joins ShaderInfo.memh: ~0 when it can't */
static u32 mh_probe(Ctx* c, u32 at, int file, u32 r) {
    int k = mh_writer(c, at, file, r);
    if (k < 0 || (u32)k >= at) {
        mh_fail(c, "a texture handle from memory whose %s%u at %u has no one writer before", mh_file_name(file), r, at);
        return SH_PROBE_NONE;
    }
    if (!mh_add(c, (u32)k, at)) return SH_PROBE_NONE;
    return SH_PROBE(k, file == MH_UR, r);
}
/* ShaderInfo.memh in program order. Each value an instruction there reads must be the one its writer left: no other
   instruction of the list between the two may write it, as the renderer runs only the list, in order */
static int mh_finish(Ctx* c) {
    u32 pos = 0;
    for (u32 k = 0; k < c->n; k++) {
        if (!c->mh_mark[k]) continue;
        c->mh_pos[k] = pos + 1;
        c->si->memh[pos][0] = c->in[k].lo;
        c->si->memh[pos][1] = c->in[k].hi;
        pos++;
    }
    c->si->nmemh = pos;
    for (u32 d = 0; d < c->nmh_dep; d++) {
        const MhDep* x = &c->mh_dep[d];
        for (u32 j = x->writer + 1; j < x->at; j++)
            if (c->mh_mark[j] && mh_may_write(&c->in[j], (int)x->file, x->reg)) {
                fail(c, "a texture handle from memory whose %s%u another of its instructions writes",
                     mh_file_name((int)x->file), x->reg);
                return 0;
            }
    }
    return 1;
}
static u32 mh_final(Ctx* c, u32 probe) {
    if (probe == SH_PROBE_NONE) return probe;
    return SH_PROBE(c->mh_pos[SH_PROBE_INSN(probe)] - 1, SH_PROBE_UNIFORM(probe), SH_PROBE_REG(probe));
}
/* the slot of a handle from memory (SNX_TEX_MEMORY): its texture word's probe, and with smp its sampler word's, with
   that view (as tex_slot's) */
static int tex_slot_memory(Ctx* c, u32 tex_probe, int smp, u32 smp_probe, u32 type, u32 array, u32 shadow, u32 ms) {
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ntex; i++) {
        const ShTex* t = &si->tex[i];
        if ((t->flags & SNX_TEX_MEMORY) && t->cbuf_off == tex_probe && t->bl2 == smp &&
            (!smp || t->bl2_off == smp_probe) && t->type == type && t->array == array && t->shadow == shadow &&
            !!(t->flags & 2) == !!ms)
            return i;
    }
    if (si->ntex >= SH_MAXTEX) {
        fail(c, "more than %d textures", SH_MAXTEX);
        return -1;
    }
    int i = si->ntex++;
    ShTex t = {0};
    t.cbuf_off = tex_probe;
    t.type = (u8)type;
    t.array = (u8)array;
    t.shadow = (u8)shadow;
    t.bindless = 1;
    t.bl2 = (u8)smp;
    t.bl2_off = smp ? smp_probe : 0;
    t.flags = (u8)(SNX_TEX_MEMORY | (ms ? 2 : 0));
    si->tex[i] = t;
    c->tex[i] = spv_emit_sampler_image(&c->e, c->t_f32, (u32)shader_vk_set(c->stage), SNX_VKB_TEX + (u32)i,
                                       type == 0 ? 1 : type, (int)array, (int)shadow, ms ? 1 : 0, &c->tex_sampled[i]);
    add_interface(c, c->tex[i]);
    return i;
}
static int tex_operands(const Sm86Insn* in, u32 n0, u32 n1, u32* regs);
/* the register of a TEX.B's handle, the first of list1 (as prepare_handles finds it): RZ when it can't tell */
static u32 tex_b_handle_reg(const Sm86Insn* in) {
    u32 type, array, coords, regs[8], lod = sm86_field(in, 87, 3);
    if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) return SM86_RZ;
    u32 n0 = array + coords, n1 = 1 + (lod == 2 || lod == 3) + sm86_field(in, 76, 1) + sm86_field(in, 78, 1);
    return tex_operands(in, n0, n1, regs) ? regs[n0] : SM86_RZ;
}
/* whether a TEX.B's handle is a constant buffer word's, which prepare_handles binds: a MOV of one, or an LDC. A handle
   with no one writer is memory_handles' too (one game's lights take theirs from memory in a loop, as a TEX.B's
   handle from an LDG was before), which then samples it as no texture */
static int tex_b_cbuf_handle(Ctx* c, u32 i, u32 reg) {
    int k = reaching_write(c, i, reg);
    for (u32 depth = 0; k >= 0 && depth < 8 && c->in[k].op == SM86_MOV && c->in[k].b.kind == SM86_SRC_REG; depth++)
        k = reaching_write(c, (u32)k, c->in[k].b.reg);
    if (k < 0) return 0;
    const Sm86Insn* w = &c->in[k];
    return (w->op == SM86_MOV && w->b.kind == SM86_SRC_CBUF && !w->b.bindless) || w->op == SM86_LDC;
}
/* The handles from memory: TEX, TLD, TLD4 and TXQ with the handle in a uniform register pair (URn the texture's word,
   URn + 1 the sampler's), and TEX.B whose handle register no constant buffer word gives. Their slots go in
   c->handle (the slot + 1), and the instructions that make them in ShaderInfo.memh */
static void memory_handles(Ctx* c) {
    u32 n = c->n, any = 0;
    for (u32 i = 0; i < n; i++) {
        int op = c->in[i].op;
        any |= op == SM86_TEX_U || op == SM86_TLD_U || op == SM86_TLD4_U || op == SM86_TXQ_U ||
               (op == SM86_TEX_B && tex_b_handle_reg(&c->in[i]) < SM86_RZ &&
                !tex_b_cbuf_handle(c, i, tex_b_handle_reg(&c->in[i])));
    }
    if (!any) return;
    c->mh_mark = calloc((size_t)n + 1, 1);
    c->mh_pos = calloc((size_t)n + 1, sizeof *c->mh_pos);
    c->mh_new = calloc((size_t)n + 1, sizeof *c->mh_new);
    u32(*probe)[2] = malloc(((size_t)n + 1) * sizeof *probe); /* ~0 for none */
    if (!c->handle) c->handle = calloc((size_t)n + 1, sizeof *c->handle);
    if (!c->mh_mark || !c->mh_pos || !c->mh_new || !probe || !c->handle) {
        free(probe);
        fail(c, "out of memory");
        return;
    }
    memset(probe, 0xff, ((size_t)n + 1) * sizeof *probe);
    /* the values, each with the instructions that make it. One whose instructions can't be run before the draw (its
       address differs from invocation to invocation: the program loads it at a cell of a grid in world space, say)
       is sampled as no texture (SH_PROBE_NONE): its instructions are unmarked, and the draw goes on without it */
    for (u32 i = 0; i < n && !c->failed; i++) {
        const Sm86Insn* in = &c->in[i];
        int op = in->op, u = op == SM86_TEX_U || op == SM86_TLD_U || op == SM86_TLD4_U || op == SM86_TXQ_U;
        u32 reg = op == SM86_TEX_B ? tex_b_handle_reg(in) : SM86_RZ;
        if (!u && !(op == SM86_TEX_B && reg < SM86_RZ && !tex_b_cbuf_handle(c, i, reg))) continue;
        u32 ur = sm86_field(in, 40, 6), nmh = c->nmh, ndep = c->nmh_dep;
        if (u && ur + 1 >= SM86_URZ) {
            fail(c, "%s of UR%u", sm86_ops[op].name, ur);
            break;
        }
        c->nmh_new = 0;
        c->mh_why[0] = 0;
        probe[i][0] = mh_probe(c, i, u ? MH_UR : MH_R, u ? ur : reg);
        if (u && probe[i][0] != SH_PROBE_NONE) probe[i][1] = mh_probe(c, i, MH_UR, ur + 1);
        if (probe[i][0] == SH_PROBE_NONE || (u && probe[i][1] == SH_PROBE_NONE)) {
            for (u32 k = 0; k < c->nmh_new; k++) c->mh_mark[c->mh_new[k]] = 0;
            c->nmh = nmh, c->nmh_dep = ndep;
            probe[i][0] = SH_PROBE_NONE, probe[i][1] = u ? SH_PROBE_NONE : ~0u;
            c->si->memh_none++;
            if (!c->si->memh_why[0]) snprintf(c->si->memh_why, sizeof c->si->memh_why, "%s", c->mh_why);
        }
    }
    if (c->failed || !mh_finish(c)) {
        free(probe);
        return;
    }
    /* the slots, the TXQs after the others: a query takes the view of a sample of the same handle */
    for (u32 pass = 0; pass < 2 && !c->failed; pass++)
        for (u32 i = 0; i < n && !c->failed; i++) {
            const Sm86Insn* in = &c->in[i];
            int op = in->op, slot = -1;
            if (probe[i][0] == ~0u || (op == SM86_TXQ_U) != (pass == 1)) continue;
            if (op != SM86_TEX_U && op != SM86_TLD_U && op != SM86_TLD4_U && op != SM86_TXQ_U && op != SM86_TEX_B)
                continue;
            u32 tex = mh_final(c, probe[i][0]), smp = probe[i][1] == ~0u ? 0 : mh_final(c, probe[i][1]);
            int has_smp = probe[i][1] != ~0u;
            if (op == SM86_TXQ_U) {
                for (int k = 0; k < c->si->ntex && slot < 0; k++) {
                    const ShTex* t = &c->si->tex[k];
                    if ((t->flags & SNX_TEX_MEMORY) && t->cbuf_off == tex && t->bl2 == has_smp && t->bl2_off == smp)
                        slot = k;
                }
                if (slot < 0) slot = tex_slot_memory(c, tex, has_smp, smp, 1, 0, 0, 0);
            } else {
                u32 type, array, coords;
                if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) continue; /* refused where it's written */
                u32 shadow = op == SM86_TLD_U ? 0 : sm86_field(in, 78, 1),
                    ms = op == SM86_TLD_U && sm86_field(in, 78, 1);
                slot = tex_slot_memory(c, tex, has_smp, smp, type, array, shadow, ms);
            }
            if (slot >= 0) c->handle[i].slot = (u32)slot + 1;
        }
    free(probe);
}
/* The handle of each TEX.B is in a register. A MOV or an LDC of a constant buffer word gives a slot as any other. An
   LDC whose address has a register in it picks from a table: one game's lights keep 32 records each, in a constant
   buffer, and a program finds the lights of a pixel with FLO on a mask, then an IMAD of that index by the record's
   size makes the LDC's address, c[bank][x * stride + base + offset]. So the texture is chosen for each pixel, from
   an array, and the table's elements are stride bytes apart, whatever the stride (they aren't a power of two) */
static void prepare_handles(Ctx* c) {
    memory_handles(c);
    for (u32 i = 0; i < c->n && !c->failed; i++) {
        const Sm86Insn* in = &c->in[i];
        if (in->op != SM86_TEX_B || (c->handle && c->handle[i].slot)) continue; /* memory_handles' */
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
    return sm86_to_spirv_ftz(code, bytes, stage, sph, si, word_count, SH_FTZ_EMULATE);
}
u32* sm86_to_spirv_ftz(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count,
                       int can_flush) {
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
    /* FTZ as spirv_translate.c decides it: per instruction without the execution mode, or with it where some FADD,
       FMUL or FFMA keeps subnormals (the mode is the whole module's); never on a device that flushes by itself */
    int ftz_flush = 0, ftz_keep = 0;
    for (u32 i = 0; i < n; i++)
        if (insn[i].op == SM86_FADD || insn[i].op == SM86_FMUL || insn[i].op == SM86_FFMA) {
            if (sm86_field(&insn[i], 80, 1))
                ftz_flush = 1;
            else
                ftz_keep = 1;
        }
    c->soft_ftz = can_flush != SH_FTZ_NATIVE && (can_flush == SH_FTZ_EMULATE || ftz_keep);
    const int ftz_mode = ftz_flush && !ftz_keep && can_flush == SH_FTZ_MODE;
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
        /* hmma_coop's matrices, which SPV_KHR_cooperative_matrix allows in a module of the Vulkan memory model only */
        c->coop = coop_wanted(c);
        if (c->coop) {
            e->vulkan_memory_model = 1;
            spv_emit_capability(e, 5345 /* VulkanMemoryModel */);
            spv_emit_capability(e, 5346 /* VulkanMemoryModelDeviceScope: the atomics' and MEMBAR.GPU's */);
        }
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
        /* atomic_fadd's loop's word, where the device has no float atomics */
        for (u32 i = 0; i < n && !c->cas_var && !g_shader_float_atomic_add; i++)
            if ((insn[i].op == SM86_RED || insn[i].op == SM86_ATOMG || insn[i].op == SM86_ATOMG_NOU) &&
                sm86_field(&insn[i], 73, 3) == 3)
                c->cas_var = spv_emit_function_variable_init(e, reg_ptr, c->zero);
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
                int always = sm86_always(in), never = sm86_never(in);
                if (never) continue;
                if (is_brx(in->op)) { /* the block's end: SHS_SWITCH takes the target's instruction */
                    u32 index = in->op == SM86_BRXU ? urd(c, sm86_field(in, 24, 6)) : rd(c, sm86_field(in, 24, 8));
                    c->branch_target =
                        spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
                                        spv_emit_iadd_u32(e, c->t_u32, index, cu(c, brx_bias(in, i))), cu(c, 4));
                    continue;
                }
                if (in->op == SM86_BRA) { /* the block's end: SHS_IF takes its condition */
                    c->branch_cond = always ? 0 : guard(c, in);
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
                    u32 cond = guard(c, in), then = spv_emit_id(e);
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
            spv_emit_float_controls(e, fn, ftz_mode, 1, 0);
            out = spv_emit_finish(e, word_count);
        }
        spv_emit_destroy(e);
    }
    if (!out && si->unsupported) sm86_debug_dump("program", code, bytes, sph, si->first_unsupported);
    free(st);
    free(c->addr_key);
    free(c->handle);
    free(c->mh_mark);
    free(c->mh_pos);
    free(c->mh_dep);
    free(c->mh_new);
    free(c->jump_first);
    free(insn);
    free(words);
    free(c);
    return out;
}
