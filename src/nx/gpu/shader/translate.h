/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the files of the SPIR-V translator share (spirv_translate.c the program, the others a part each): the
   emitter's context, the structured flow's state, the accessors of registers, predicates and flags each
   instruction calls, and the helpers one file gives another. Internal to src/nx/gpu/shader/ */
#ifndef SNX_SHADER_TRANSLATE_H
#define SNX_SHADER_TRANSLATE_H
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include "nx/gpu/shader/translator.h"
#include <assert.h>
#include <stdlib.h>

enum { VAR_PRED = 255, VAR_FLAG = VAR_PRED + 7, VAR_COUNT = VAR_FLAG + 4 };

typedef struct {
    SpvEmitter emit;
    u32 u32_type, f32_type, f16_type, f64_type, bool_type, zero, true_value, false_value, glsl;
    u32 reg[255];
    /* The registers, predicates and flags (VAR_PRED + p, VAR_FLAG + k) as values in the block being written, so that
       reading one again takes no load and only its last value in the block is stored: the value (0 for a register
       written as reg_alt only, bitcast to its bits when read so), the emitter's block it was read or written in, and
       whether the variable still has to take it. Each is stored before the branch or merge that ends its block
       (shader_var_flush, the emitter's block_end): blocks that follow read the variables. A register's bitcast to another
       32-bit type in the block is reg_alt, of reg_alt_type */
    u32 var_val[VAR_COUNT], var_block[VAR_COUNT], reg_alt[255], reg_alt_type[255];
    u8 var_dirty[VAR_COUNT];
    u8* ftz; /* a bit per id: values with no subnormal (shader_float_ftz's, and reads of registers not tainted) */
    size_t ftz_bytes;
    /* each value's flush in the block it was made in (by id, ftz_of_cap of them): a value read once and used by
       several FTZ instructions is flushed once a block */
    u32 *ftz_of, *ftz_of_block;
    size_t ftz_of_cap;
    /* Registers that may hold a subnormal on the per-instruction FTZ path: written by anything but an FTZ float
       instruction whose result is flushed. A float read of one that isn't is flushed already */
    u8 tainted[256];
    int taint_known;
    int ftz_float; /* the module preserves signed zeros (SignedZeroInfNanPreserve): float_ftz flushes with a float test */
    int soft_ftz_on; /* FTZ instructions flush their operands and results with shader_float_ftz (SH_FTZ_EMULATE's way) */
    /* A pure instruction's guard while it is translated: it runs unguarded and each register, predicate and flag it
       writes keeps its old value where the guard is false, in place of a branch around it. 0 otherwise */
    u32 guard;
    u32 pred[7];
    u32 cc_c, cc_o, cc_z, cc_s;
    u32 cbuf[18], cbuf_pointer[18];
    /* a vertex program's reads of NVN's draw parameters in bank 0 (shader.h SH_CB0_): the push block and pointer type
       whose z says which come from BaseVertex and BaseInstance, and those inputs. 0 when it reads neither */
    u32 dp_push, dp_push_pointer, dp_base_vertex, dp_base_instance;
    /* constant buffer words read in the block being written, by bank and offset (a cache: a word that collides is
       read again), and their bitcast to a float, 0 until made. A draw's constant buffers don't change under it */
    struct {
        u32 key, block, word, as_float;
    } cbuf_seen[512];
    u32 scale_buffer, scale_pointer;
    u32 sampler[SH_MAXTEX], sampled_type[SH_MAXTEX];
    u32 tex_index[SH_MAXTEX], tex_map_buffer, tex_map_pointer, active_index;
    u8 tex_table[SH_MAXTEX]; /* indexed texture slot to descriptor table */
    /* an indexed slot's array, the one of its kind, and a shadow kind's array of depth views with their type */
    u32 tex_array[SH_MAXTEX], tex_depth[SH_MAXTEX], tex_depth_type[SH_MAXTEX];
    u32 tex_func_buffer, tex_func_pointer;
    u32 depth_type, depth_sampler; /* the swapped slot's depth type and element, 0 for a kind with no depth views */
    u32 swapped;                   /* 1 + the slot whose sampler is an array element for this instruction */
    u32 image[8], image_type[8];
    u32 gbuf[16], gbuf_pointer, smap, subgroup_lane_input;
    int quad; /* quad operations are declared: shader_shfl_quad's SHFL are ones */
    /* A fragment program with flow discards nothing inside its loop: a KIL or a failed alpha test sets this bool,
       an EXIT ends the loop for the lane, and the lane is discarded after it. A demote or a kill in the loop lets a
       driver drop a quad's remaining helpers from execution while they still hold the loop open: AMD's Windows
       compiler then repeats the loop with no lane left to end it, until the GPU is reset. */
    u32 discarded;
    u32 branch_cond;   /* with structured flow, the condition of the branch that ends the block, 0 for always */
    u32 branch_target; /* and the word a BRX that ends it goes to */
} DirectCtx;

/* Fragment fetches from a scaled render target (resolution=): texel p is at (p + 0.5) * scale, and TXQ answers the
   size over the scale. The texture's bit is in the push constants' uvec4, x for slots 0-31, y for 32-63 */
_Static_assert(SH_MAXTEX <= 64, "the scaled textures' mask has a bit a slot");

static const SpvGlsl450Op conversion_rounds[] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};

/* the words of a program: the 0x20000-byte window vk_shader_get reads it from (SH_CODE), which direct_can_reach's
   reachability bitset holds */
#define DIRECT_WORDS_MAX 16384u

/* gb_at's mark for an access whose buffer is found when it runs (shader_global_dynamic) */
#define GB_DYNAMIC 254

/* insn: code's words as host numbers (shader_words) */
/* Writing a program's structured control flow (shader_structure): the labels of each scope (header, body, continue
   target and merge, made when it opens), and the open selections */
typedef struct {
    const ShStructure* st;
    u32 next;       /* the token to write next */
    u32* label;     /* 4 a scope, then the selections' merge and else labels, 2 each */
    u8* used;       /* a scope's merge or a selection's merge has a branch to it: a byte each */
    u32 selections; /* open */
    u32 flag;       /* the Function variable of the pending exit: the scope + 1 a break goes on to, 0 for none */
    int open;       /* a block is being written: no branch or return has ended it */
    u32 body_start, body_end;
} StructEmit;

enum { SE_DONE, SE_BODY, SE_END };

/* The fixed function's varyings: the front and back colors (0x280-0x2bf), the fog coordinate (0x2e8) and the 10
   texture coordinates (0x300-0x39f). SPIR-V has none of them, so each group (a color, the fog, a coordinate: 15 of
   them) takes generic 17 + its index in every stage. Eden and Ryujinx give each one of the generics a program leaves
   unused, matched to the previous stage's choice; a program here is translated without the other stages, so the
   choice is fixed, the last 15 generics, and a program that uses one of those itself as well is refused. A group's
   index, or -1 for an address of none */
#define LEGACY_GENERIC 17u

/* registers.c */
void shader_var_flush(void* user);
u32 shader_ftz_read(DirectCtx* c, const u64* insn, const u8* block_start, u32 i, u32 r, u32 value);
void shader_flags_logic(DirectCtx* c, u32 value);
void shader_flags_logic_extended(DirectCtx* c, u32 value, int extended);
u32 shader_cc_test(DirectCtx* c, u32 code);
int shader_ftz_has(const DirectCtx* c, u32 value);
void shader_ftz_mark(DirectCtx* c, u32 value);
u32 shader_float_ftz(DirectCtx* c, u32 value);
/* forms.c */
u32 shader_global_atomic_opcode(u64 w, int reduction);
u32 shader_shared_atomic_opcode(u64 w);
int shader_pure_op(int op);
int shader_ftz_writer(u64 w);
int shader_may_write(u64 w, u32 r);
int shader_flow_has_cc(int op);
int shader_helper_side_effect(int op);
int shader_gbuf_tag(int stage, u32 index, u32 offset);
int shader_gbuf_tag_any(int stage, u32 index, u32 offset);
int shader_texs_outputs(u64 w, u32 regs[4], u32 comps[4]);
void shader_texs_mark_outputs(u64 w, u8 used[255]);
int shader_writes_reg(u64 w, int op, u32 reg);
u32 shader_direct_pred_writes(u64 w, int op);
u32 shader_next_instruction(u32 index, u32 limit);
int shader_shfl_quad(u64 w, u32* lane);
u32 shader_fswzadd_lanes(u64 w, u32 mode);
int shader_predicate_comparison(int op);
int shader_writes_predicate(u64 w, int op, u32 p);
int shader_reads_predicate(u64 w, int op, u32 p);
int shader_supported(u64 w, int op);
int shader_stage_has(u64 w, int op, int stage);
int shader_texs_type(u32 encoding);
int shader_texs_array(u32 encoding);
int shader_texs_shadow(u32 encoding);
u32 shader_texs_y(u32 encoding, u32 a, u32 b);
int shader_texs_explicit(u32 encoding);
u32 shader_texs_level(u32 encoding, u32 b);
u32 shader_texs_dref(u32 encoding, u32 b);
int shader_half_arithmetic(int op);
u32 shader_sparse_predicate(u64 w, int op);
int shader_legacy_group(u32 addr);
u32 shader_legacy_address(u32 addr);
u32 shader_legacy_base(u32 g);
/* dataflow.c */
int shader_direct_own_pixel_fetch(const u64* insn, u32 n, u32 index);
int shader_direct_track_const(const u64* insn, u32 n, u32 index, u32 reg, u32* value, int depth);
int shader_direct_track_image_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, int depth);
int shader_direct_track_image_handle_pair(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth);
int shader_al2p_predicate_unseen(const u64* insn, u32 n, u32 at);
u32 shader_direct_indexed_handle(const u64* insn, u32 n, u32 index, u32 reg, int stage, u32* ldc);
int shader_direct_bindless_matches(const ShTex* t, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed,
                                   u32 ldc);
int shader_direct_query_type(const u64* insn, u32 n, int bindless, u32 bank, u32 off, u32* type, u32* array);
u32* shader_untracked_handle(ShaderInfo* si, const char* kind, const u64* insn, u32 i, u32 reg);
u32* shader_untracked_reject(ShaderInfo* si, const u64* insn, u32 i, u32 reg, u8* gb_at, u32* pop_targets);
int shader_legacy_varyings(const u64* insn, u32 n, u32 words, const u32* sph, int stage, ShaderInfo* si, u64** moved,
                           u32* fs_groups);
/* emit_int.c */
u32 shader_vm_select(DirectCtx* c, u32 i32_type, u32 value, u32 select);
u32 shader_video_operand(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 value, u32 select);
u32 shader_video_finish(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 result, u32 third, u32 mode,
                        int saturate, int dsigned, int unsigned_nonnegative);
u32 shader_xmad_half(DirectCtx* c, u32 i32_type, u32 value, int high, int is_signed);
u32 shader_subgroup_ballot_word(DirectCtx* c, u32 vec4_u32, u32 lane, u32 predicate);
u32 shader_integer_add(DirectCtx* c, u32 left, u32 right, u32 avg, int extended, int write_cc);
u32 shader_lane_dead(DirectCtx* c);
u32 shader_integer_compare(DirectCtx* c, u32 i32_type, u64 w, u32 left, u32 right);
u32 shader_integer_right(DirectCtx* c, u64 w, int kind);
u32 shader_integer_mul_high(DirectCtx* c, u32 lhs, u32 rhs, int signed_lhs, int signed_rhs);
u32 shader_prmt_byte(DirectCtx* c, u32 low, u32 high, u32 idx);
/* emit_float.c */
u32 shader_float_mod(DirectCtx* c, u32 value, int absolute, int negative);
u32 shader_double_read(DirectCtx* c, u32 vec2_u32, u32 index);
u32 shader_double_mod(DirectCtx* c, u32 value, int absolute, int negative);
void shader_double_write(DirectCtx* c, u32 vec2_u32, u32 index, u32 value);
u32 shader_half_pair(DirectCtx* c, u32 vec2, u32 raw, u32 swizzle);
u32 shader_half_pair_mod(DirectCtx* c, u32 vec2, u32 pair, int absolute, int negative);
u32 shader_half_immediate(DirectCtx* c, u64 w);
u32 shader_half_fmz(DirectCtx* c, u32 vec2, u32 pair, u32 lhs, u32 rhs, u32 zero_value);
u32 shader_float_saturate(DirectCtx* c, u32 value, int saturate);
u32 shader_half_pair_result(DirectCtx* c, u32 vec2, u32 pair, u32 old, u32 merge, int saturate);
u32 shader_float_imm20(DirectCtx* c, u64 w);
u32 shader_float_imm32(DirectCtx* c, u64 w);
u32 shader_conversion_source_f32(DirectCtx* c, u32 vec2, u64 w, int kind, u32 format);
u32 shader_double_source(DirectCtx* c, u32 vec2_u32, u64 w, int kind);
/* emit_memory.c */
u32 shader_shared_index(DirectCtx* c, u32 index, u32 words);
u32 shader_wrap_step(DirectCtx* c, u32 old, u32 value, int decrement);
u32 shader_atomic_wrap_loop(DirectCtx* c, u32 old_var, u32 pointer, u32 value, int decrement);
u32 shader_cbuf_word(DirectCtx* c, u64 w);
u32 shader_cbuf_read(DirectCtx* c, u64 w);
u32 shader_cbuf_read_dynamic(DirectCtx* c, u32 bank, u32 address);
u32 shader_gbuf_word_index(DirectCtx* c, int stage, u32 slot, u32 word);
u32 shader_global_atomic_pointer(DirectCtx* c, int stage, u32 slot, u32 base_off, u32 a, u64 w);
u32 shader_shared_atomic_index(DirectCtx* c, u32 a, u64 w, u32 shared_words);
void shader_global_dynamic(DirectCtx* c, int stage, u64 w, int op, const u32* gbuf_off, int ngbuf);
/* emit_texture.c */
u32 shader_array_layer(DirectCtx* c, u32 r);
u32 shader_texture_component(DirectCtx* c, u32 sampled, u32 scalar_type, u32 component);
int shader_dc_color_func(const ShaderInfo* si, int slot);
u32 shader_dc_compare(DirectCtx* c, int func, u32 reference, u32 texel);
u32 shader_indexed_is_depth(DirectCtx* c, int slot);
u32 shader_dc_compare_indexed(DirectCtx* c, int slot, u32 reference, u32 texel);
u32 shader_sample_dref(DirectCtx* c, const ShaderInfo* si, int slot, u32 vec4_type, u32 coords, u32 dref, int use_lod,
                       u32 lod);
void shader_scaled_coords(DirectCtx* c, u32 push, int slot, int size, u32 i32_type, u32* x, u32* y);
void shader_rect_coords(DirectCtx* c, u32 push, int scaled, int slot, u32 vec2_u32, u32 i32_type, u32* x, u32* y,
                        u32* gx, u32* gy);
void shader_texel_row_coord(DirectCtx* c, u32 i32_type, u32* x, u32* y);
int shader_direct_image_slot(const ShaderInfo* info, u32 off, int bindless, u32 bank, int r32ui, int type);
int shader_direct_query_slot(const ShTex* textures, int count, u32 off, int bindless, u32 bank, int handle_count,
                             u32 bank2, u32 off2, u32 indexed, u32 ldc);
int shader_direct_typed_slot(const ShTex* textures, int count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                             int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc);
int shader_direct_add_typed_slot(ShTex* textures, int* count, u32 off, u32 type, u32 array, u32 shadow, u32 kind,
                                 int bindless, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed, u32 ldc);
void shader_direct_swap_element(DirectCtx* c, int slot);
u32 shader_texture_float_component(DirectCtx* c, u32 value, u32 scalar, u32 i32_type);
u32 shader_pack_texture_halves(DirectCtx* c, u32 vec2, u32 first, u32 second);
u32 shader_direct_offset_coords(DirectCtx* c, u32 coords, u32 offset, u32 coord_count, u32 offset_count, int slot,
                                u32 level, u32 vec2, u32 vec3, u32 vec2_i32, u32 vec3_i32, u32 i32_type);
u32 shader_direct_gather_offset(DirectCtx* c, u32 raw, u32 shift, int zero_y, u32 i32_type, u32 vec2_i32);
void shader_texs_store(DirectCtx* c, u64 w, u32 sample, u32 scalar, int shadow, u32 i32_type, u32 vec2, u32 zero_f);
/* emit_io.c */
u32 shader_ipa_interpolation(const u32* sph, u32 input);
void shader_alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha);
u32 shader_frag_y_origin(DirectCtx* c, u32 push, u32 y);
void shader_vertex_depth_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type);
void shader_xfb_store(DirectCtx* c, u32 vec4, const u32* output_attrs, const u8* attr_private, u32 (*components)[4]);
void shader_geometry_passthrough(DirectCtx* c, const ShaderInfo* si, u32 vertices, u32 vec4, u32 position,
                                 u32 position_in, const u32* outputs, const u32* inputs, u32 point_size, u32 layer,
                                 u32 layer_type, u32 clip, u32 clip_count, u32 primitive_id, u32 primitive_id_out,
                                 int depth_fix, u32 push, u32 push_pointer);
/* emit_flow.c */
void shader_dynamic_pop(DirectCtx* c, u32 stack_ptr, const u32 stack[32], u32 kind_value, u32 fallback, u32* found,
                        u32* destination, u32* new_sp);
int shader_structure_step(DirectCtx* c, StructEmit* se);

/* the Function variable of a register, predicate or flag (z, s, c, o) */
static inline u32 var_of(const DirectCtx* c, u32 v) {
    if (v < VAR_PRED) return c->reg[v];
    if (v < VAR_FLAG) return c->pred[v - VAR_PRED];
    v -= VAR_FLAG;
    return v == 0 ? c->cc_z : v == 1 ? c->cc_s : v == 2 ? c->cc_c : c->cc_o;
}
static inline u32 flag_var(const DirectCtx* c, u32 variable) {
    return VAR_FLAG + (variable == c->cc_z ? 0 : variable == c->cc_s ? 1 : variable == c->cc_c ? 2 : 3);
}

/* a predicate or flag's value: its variable's, loaded once a block */
static inline u32 var_read(DirectCtx* c, u32 v) {
    if (c->var_val[v] && c->var_block[v] == c->emit.block) return c->var_val[v];
    c->var_val[v] = spv_emit_load(&c->emit, c->bool_type, var_of(c, v));
    c->var_block[v] = c->emit.block;
    c->var_dirty[v] = 0;
    return c->var_val[v];
}
static inline void var_write(DirectCtx* c, u32 v, u32 value) {
    c->var_val[v] = value;
    c->var_block[v] = c->emit.block;
    c->var_dirty[v] = 1;
}

static inline u32 reg_read(DirectCtx* c, u32 r) {
    if (r >= 255) return c->zero;
    if (c->var_block[r] == c->emit.block && (c->var_val[r] || c->reg_alt_type[r])) {
        if (!c->var_val[r]) c->var_val[r] = spv_emit_bitcast(&c->emit, c->u32_type, c->reg_alt[r]);
        return c->var_val[r];
    }
    c->var_val[r] = spv_emit_load(&c->emit, c->u32_type, c->reg[r]);
    c->var_block[r] = c->emit.block;
    c->var_dirty[r] = 0;
    c->reg_alt_type[r] = 0;
    return c->var_val[r];
}

static inline void reg_write(DirectCtx* c, u32 r, u32 value) {
    if (r >= 255) return;
    if (c->guard) value = spv_emit_select(&c->emit, c->u32_type, c->guard, value, reg_read(c, r));
    var_write(c, r, value);
    c->reg_alt_type[r] = 0;
}

/* value, of another 32-bit type, stored as the register's bits: a read of that type in the block takes value back,
   and its bits are a bitcast made when something reads or stores them */
static inline void reg_write_as(DirectCtx* c, u32 r, u32 value, u32 type) {
    if (r >= 255) return;
    if (c->guard) { /* a guarded write's bits are a select of value's, not value's */
        reg_write(c, r, spv_emit_bitcast(&c->emit, c->u32_type, value));
        return;
    }
    var_write(c, r, 0);
    c->reg_alt[r] = value;
    c->reg_alt_type[r] = type;
}
static inline u32 reg_read_as(DirectCtx* c, u32 r, u32 type) {
    if (r >= 255) return spv_emit_bitcast(&c->emit, type, c->zero);
    if (c->var_block[r] == c->emit.block && c->reg_alt_type[r] == type) return c->reg_alt[r];
    u32 bits = reg_read(c, r);
    c->reg_alt[r] = spv_emit_bitcast(&c->emit, type, bits);
    c->reg_alt_type[r] = type;
    if (type == c->f32_type && c->taint_known && !c->tainted[r]) shader_ftz_mark(c, c->reg_alt[r]);
    return c->reg_alt[r];
}

static inline u32 pred_read(DirectCtx* c, u32 p, int negative) {
    u32 value = p == 7 ? c->true_value : var_read(c, VAR_PRED + p);
    return negative ? spv_emit_bool_not(&c->emit, c->bool_type, value) : value;
}

static inline void pred_write(DirectCtx* c, u32 p, u32 value) {
    if (p >= 7) return;
    if (c->guard) value = spv_emit_select(&c->emit, c->bool_type, c->guard, value, pred_read(c, p, 0));
    var_write(c, VAR_PRED + p, value);
}

/* a condition code flag (its variable, c->cc_z...): its value, and written, kept as it was where a pure
   instruction's guard is false */
static inline u32 flag_read(DirectCtx* c, u32 flag) { return var_read(c, flag_var(c, flag)); }
static inline void cc_store(DirectCtx* c, u32 flag, u32 value) {
    if (c->guard) value = spv_emit_select(&c->emit, c->bool_type, c->guard, value, flag_read(c, flag));
    var_write(c, flag_var(c, flag), value);
}

static inline u32 bool_combine(DirectCtx* c, u32 op, u32 left, u32 right) {
    return op < 3 ? spv_emit_bool_binary(&c->emit, c->bool_type, op, left, right) : left;
}
static inline u32 cc_not(DirectCtx* c, u32 x) { return spv_emit_bool_not(&c->emit, c->bool_type, x); }
static inline u32 cc_and(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 0, x, y); }
static inline u32 cc_or(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 1, x, y); }
static inline u32 cc_xor(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 2, x, y); }

static inline u32* direct_reject(ShaderInfo* si, const char* reason) {
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
    return NULL;
}

static inline u32* direct_reject_with_buffers(ShaderInfo* si, const char* reason, u32* pop_targets, u8* gb_at) {
    free(gb_at);
    free(pop_targets);
    return direct_reject(si, reason);
}
/* What to_spirv sets up before it writes the program's instructions and each family's function reads
   (emit_flow, emit_io...): the types, the interface variables, the resource slots and the facts its prescan
   found, each named as to_spirv names it, an array by its first element */
typedef struct {
    ShaderInfo* si;
    int stage;
    const u64* insn;
    u32 n;
    u32* sph;
    u32 target;
    int passthrough;
    int point_size_used;
    int between;
    u32 in_vertices;
    u32 out_vertices;
    u32 clip_used;
    ShTex* textures;
    int texture_count;
    ShaderInfo* image_info;
    int needs_scale;
    int needs_texel_scale;
    int has_pexit;
    int lane_model;
    int lane_is_invocation;
    u32* pop_targets;
    int dynamic_stack;
    u8* gb_at;
    int ngbuf;
    u32* gbuf_off;
    u64 rect_slots;
    DirectCtx* c;
    u32 i32_type;
    u32 i64_type;
    u32 u64_type;
    u32 vec4;
    u32 vec4_u32;
    u32 vec4_i32;
    u32 vec2;
    u32 vec2f16;
    u32 vec2_u32;
    u32 vec2_i32;
    u32 vec3;
    u32 vec3_u32;
    u32 vec3_i32;
    u32 zero_f;
    u32* inputs;
    u32* outputs;
    u32* input_component_type;
    u32* input_vec_type;
    u32 position;
    u32* output_attrs;
    u32 point_size;
    u32 layer_output;
    u32 layer_input;
    u32 viewport_output;
    u8* attr_private;
    u32 (*xfb_components)[4];
    int xfb_on;
    u32 clip_output;
    u32 clip_count;
    u32 depth_output;
    u32 position_in;
    u32 primitive_id;
    u32 primitive_id_out;
    u32* patch_vars;
    u32 tess_outer;
    u32 tess_inner;
    u32 tess_coord;
    u32 invocation_id;
    u32 patch_vertices;
    int depth_fix;
    u32 sample_mask_output;
    u32 sample_id;
    u32 coverage;
    u32 fragcoord;
    u32 frontface;
    u32 point_coord;
    u32 instance_index;
    u32 base_instance;
    u32 vertex_index;
    u32 local_input;
    u32 group_input;
    u32* texture_vec4;
    u32* texture_scalar;
    u32 shared_words;
    u32 shared_element_pointer;
    u32 shared_array;
    u32 push;
    u32 push_pointer;
    u32 local_words;
    u32 local_element_pointer;
    u32 local_array;
    u32 atomic_old_var;
    u32 stack_ptr;
    u32* stack;
    u32 call_ptr;
    u32* call_stack;
    u32 pc;
    u8* block_start;
    ShStructure* structure;
} Tr;
/* each family of instructions: writes the instruction i (its word w, opcode op, and fields d, a, b) when one of its branches takes it; op_family (spirv_translate.c) sends each opcode to its family */
void emit_flow(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_io(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_thread(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_int(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_float(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_memory(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
void emit_texture(const Tr* t, u32 i, u64 w, int op, u32 d, u32 a, u32 b);
#endif
