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
#include "nx/gpu/shader/warp_notes.h"
#include "nx/gpu/shader/fuse.h"
#include "nx/gpu/shader/minmax_spirv.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/tracy.h"
#include "common/platform/time.h"
#include <stdarg.h>

/* The translation phases timed for the trace's dominant-phase detail. */
enum { P86_DECODE, P86_DATAFLOW, P86_STRUCTURE, P86_ALLOC, P86_EMIT, P86_N };
static const char* const sm86_phase_name[P86_N] = {"decode", "dataflow", "structure", "alloc", "emit"};

#define MAX_INTERFACES 256

/* What prepare_handles found, by instruction. A TEX.B: its slot + 1, and for an indexed handle its LDC + 1. That LDC:
   the variable that keeps the element it picks, the table's first byte, the bytes between its elements */
typedef struct {
    u32 slot, ldc, var, first, stride;
} TexHandle;
/* HMMA on the device's cooperative matrices (hmma_coop): the matrices a run of HMMAs takes again. A fragment is a
   matrix whose words were staged from a tuple of registers, and a D is one whose words no register holds yet. Both
   hold only while their registers keep their values, which coop_gen counts, and only within one block: a matrix
   defined in one arm of a branch does not reach the merge, so coop_flow empties them at a branch or a label. The
   bounds are the fragments of a GEMM's step (2 A, 4 B) and its accumulators (8) */
#define COOP_FRAGS 8
#define COOP_ACC   8
typedef struct {
    u32 first, n;    /* the tuple: its first register and its length */
    u32 shape;       /* the shape they were staged as (COOP_SHAPE_*) */
    u32 b;           /* 0 for A, 1 for B */
    u32 gen[4];      /* coop_gen of each register when they were staged */
    u32 nmat;        /* the matrices, one for each M block and k half */
    u32 mat[4];
} CoopFrag;
typedef struct {
    u32 first, n;    /* the tuple of the f32 D: 4 registers */
    u32 shape;
    u32 gen[4];
    u32 nmat;        /* the matrices, one for each M block */
    u32 mat[2];
} CoopAcc;
/* the jump table of a BRX, as brx_resolve found it: the instructions that its entries name. state is 0 before the
   search, 1 when count entries were found (in targets, malloc'd), and 2 when no table reaches the BRX on every path
   (c->failed says why). The targets are not kept by instruction: the code window is 0x80000 bytes, so 64 words for
   each of its 32768 instructions would be 8.6 MB of zeroing for a program that may have no BRX at all */
typedef struct {
    u32 count;
    u32* targets;
    u8 state;
} BrxTable;
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
    u32 t_u16; /* narrow LDSM shared loads (hmma_narrow_lds) */
    u8 narrow_unsafe; /* a shared atomic or sub-word shared store the u16 view cannot mirror */
    u32 shared16, shared16_ptr, shared16_words; /* the u16 view of shared memory, kept in sync by its stores */
    u32 zero, true_v, false_v, glsl;
    /* the program's state: Function variables, made at the start */
    u32 reg[256], ureg[64], pred[8], upred[8];
    /* resources, made when first used */
    u32 cbuf[18], cbuf_ptr, root;
    u32 gbuf[16], gbuf_ptr, smap;
    u32 gbuf_f32[16];  /* each global buffer's view as floats, for OpAtomicFAddEXT (atomic_fadd) */
    /* gbuf_address in this program (gbuf_by_address): gbuf[] are ids that only name the slots, and no storage buffer
       is declared for them */
    int gbuf_addressed;
    u32 cas_var;       /* atomic_fadd's loop's word, made at the start where the device has no float atomics */
    u32 wpages, mbase; /* SNX_VKB_WPAGES, and SNX_VKB_MBASE (the windows of pointer tables, SH_GBUF_TABLE) */
    u32 gkey;         /* the global buffer the last global_address named */
    u32 shared, shared_ptr, shared_words;
    u32 local, local_ptr, local_words; /* LDL and STL's memory: a Function array, made at the start */
    u32 in_attr[32], out_attr[32], position, point_size, fragcoord, color[8], depth;
    /* a vertex program of vertex_fetch: each component of the attributes it reads, as bits, from its prologue
       (vertex_fetch_prologue), which dominates all its code */
    u32 fetched[32][4], fetched_mask;
    u32 vertex_index, instance_index, base_instance, local_id, group_id, lane_id;
    u32 num_groups, hmma_count;  /* NumWorkgroups and the HMMAs so far, for hmma_zero */
    int coop;                    /* HMMA takes hmma_coop (coop_wanted), and the module the Vulkan memory model */
    u32 coop_slots, subgroup_id; /* hmma_coop's slots (Workgroup) and SubgroupId */
    CoopFrag coop_frags[COOP_FRAGS]; /* the staged fragments this block's HMMAs can take again */
    CoopAcc coop_acc[COOP_ACC];      /* the D matrices this block's words are not written for yet */
    u32 nfrags, nacc;
    u32 coop_gen[256];               /* the writes each register took, which the matrices are held against */
    u32 stage_slots;                 /* hmma's A and B staged a warp each (g_spirv_translate.hmma_stage, Workgroup) */
    u32 warp_base;                   /* warp_read's index high bits, made once a block (coop_flow forgets it) */
    u32 push, push_ptr; /* a compute dispatch's push constant vec4 (push_component): a task program's first workgroup */
    /* with tex_table: the words' uniform buffer, the samplers' table, and each kind of image's table (table_kind) */
    u32 tex_words, tex_words_ptr, table_samplers;
    u32 map, map_ptr; /* with pool_map and no device address: the map's storage buffer (map_word) */
    u32 table_kinds, table_kind_key[32], table_kind_var[32], table_kind_sampled[32];
    u32 tex_images[SH_MAXTEX]; /* a table slot's image table, 0 for a slot of its own variable */
    u32 centroid_mask;  /* the inputs decorated Centroid */
    u32 legacy_fs;      /* a fragment program's legacy varying groups (shader_legacy_group), read as generics 17-31 */
    u32 point_coord, front_facing, primitive_id, layer_in;
    u32 frag_size; /* FragmentSizeNV or ShadingRateKHR (g_spirv_translate.shading_rate) */
    /* a geometry program: its input position (an array of the input primitive's vertices, as its in_attr are), its
       PrimitiveId in and out (passthrough), the viewport mask it writes (a Function variable, 1 until written) and
       whether its SPH asks for passthrough */
    u32 position_in, primitive_id_in, primitive_id_out, viewport_mask;
    u32 layer_out; /* the layer's output (BuiltIn Layer), made at its first store */
    /* a vertex or geometry program's ClipDistance output, 8 planes (clip_var) */
    u32 clip;
    int passthrough;
    /* a fragment program at a resolution scale: SNX_VKB_RS's vec4 (scale_component) */
    u32 scale, scale_ptr;
    /* a graphics stage's per-draw block (SNX_VKB_DRAW, draw_component) and the push constant word 0 that says where it
       starts (draw_at) */
    u32 draw, draw_ptr, draw_at, draw_at_ptr;
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
    /* the storage buffers whose words are pointer tables (SH_GBUF_TABLE_SSBO), by their keys: a register's table is
       TAB_SSBO + its index there. Those addresses come from, a bit each, and the bytes the accesses reach past a
       pointer (ShaderInfo.table_reach), or reach_open where one adds an offset it computes */
    u32 table_ssbo[4], ntable_ssbo, table_ssbo_used, table_reach;
    int table_reach_open;
    u32 pairs[8][2], npairs; /* the two constant buffer bases of an address that adds both (track_step) */
    u32* jump_first; /* by instruction, the first of the BRAs and BRX that go to it in jump_from (n + 1 words), and */
    u32* jump_from;  /* those instructions: made the first time reg_const_at needs them (flow_sources) */
    BrxTable* brx_table; /* by instruction, the jump table that brx_resolve found for its BRX */
    int brx_resolved;    /* brx_resolve has looked for the tables */
    u32 cur;         /* the instruction being written */
    u32 nblocks, nbranches; /* the program's blocks and branches, for the trace's plots */
    /* the registers an IPA that ipa_over_w takes writes, and those a MUFU.RCP writes, a bit each, made at the first
       FMUL fmul_over_w looks at: an FMUL of two others needs no search */
    u32 over_w_ipa[8], over_w_rcp[8];
    int over_w_known;
    u32 branch_cond; /* the condition of the BRA that ends the body before, 0 for always */
    u32 branch_target; /* the instruction that the BRX which ends the body before goes to */
    /* a task or mesh program's ISBE (isbe_var): a mesh program's attribute area and map (Workgroup), a task
       payload (TaskPayloadWorkgroupEXT), their words, and LocalInvocationIndex */
    u32 isbe_attr, isbe_map, payload, isbe_attr_words, isbe_map_words, payload_words, local_index;
    /* the compute-task path (ShMesh.payload_buf, sm86.h): the buffer (a flat u32 array of SNX_VKB_MESHBUF) and the
       words of a task workgroup's slot */
    u32 payload_block, payload_stride, payload_groups;
    int payload_buf;
    u32 mesh_first;                    /* the uniform buffer of an indirect mesh draw's first group (workgroup_index) */
    u32 vertex_words, primitive_words; /* the output words of a vertex and of a primitive (omap_words) */
    u32 prim_attr[32];                 /* a mesh program's per-primitive generic outputs */
    int soft_ftz;                      /* FTZ instructions flush their operands and results here (sm86_to_spirv_ftz) */
    int lanes;                         /* what the stage's subgroups give (SM86_LANES_) */
    int lone; /* the stage has no warp of 32 lanes: an invocation is one alone, lane 0 (lane_gate) */
    ShNotes* notes; /* a compute program's, whose proof sm86_lone.c makes: NULL in others */
    ShNotes lone_notes;
    /* a write of register cap_reg while cap_on is kept in cap_val (cap_n of them) instead of stored: the halves of a
       complementary pair (pair_partner), which become one select */
    int cap_on;
    u32 cap_reg, cap_val, cap_n;
    /* by instruction, 1 where a fusion of sm86.roots (sg86_fuse) took it with the ones before it, which it wrote:
       made with the first instruction a fusion takes, NULL before */
    u8* fused;
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
    SNX_MSG(c->si->first_unsupported, strlen(c->si->first_unsupported));
    SNX_PLOT("shader translate failures", 1);
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
/* PackHalf2x16 of a vec2 of f32, as the console converts f32 to f16. Where the device may flush f16 subnormals
   (g_spirv_translate.half_flush) a half below 2^-14 is RoundEven(|x| 2^24), the subnormal's own bits, and x's sign:
   2^24 scales without rounding and the product is below 1024, so it is the f16 for every such f32 */
static u32 pack_half(Ctx* c, u32 pair) {
    u32 bits = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_PACK_HALF2X16, pair);
    if (!g_spirv_translate.half_flush) return bits;
    for (u32 k = 0; k < 2; k++) {
        u32 x = spv_emit_vec4_extract(&c->e, c->t_f32, pair, k);
        u32 a = SPV_EXT(&c->e, c->t_f32, SPV_GLSL_F_ABS, x);
        u32 small = op2(c, SPV_OP_F_ORD_LESS_THAN, c->t_bool, a, cf(c, 0x1p-14f));
        u32 m = spv_emit_unary(
            &c->e, SPV_OP_CONVERT_F_TO_U, c->t_u32,
            SPV_EXT(&c->e, c->t_f32, SPV_GLSL_ROUND_EVEN, op2(c, SPV_OP_F_MUL, c->t_f32, a, cf(c, 0x1p24f))));
        u32 sign = op2(c, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
                       op2(c, SPV_OP_BITWISE_AND, c->t_u32, to_u(c, x), cu(c, 0x80000000u)), cu(c, 16));
        u32 half = op2(c, SPV_OP_BITWISE_OR, c->t_u32, m, sign);
        u32 kept = op2(c, SPV_OP_BITWISE_AND, c->t_u32, bits, cu(c, k ? 0xffffu : 0xffff0000u));
        u32 exact = op2(c, SPV_OP_BITWISE_OR, c->t_u32, kept,
                        k ? op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, half, cu(c, 16)) : half);
        bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, small, exact, bits);
    }
    return bits;
}

/* ---- registers ---- */

/* hmma_coop keeps a D matrix whose words its registers do not have, so a read or a write of one of them makes the
   words first (coop_acc_take, defined with the matrices below), and a run of HMMAs can take the registers' matrices
   again while they are as they were, which coop_gen counts. Both, and the fragments, hold in one block only: the
   flow instructions of the module take a block's matrices to their words at its end (coop_flow) */
static void coop_acc_take(Ctx* c, u32 r);
static void coop_flow(Ctx* c);
/* the flow instructions of the module, which take the matrices of the block to their words first: a matrix is a value
   in one block only */
static void coop_label(Ctx* c, u32 label) {
    coop_flow(c);
    spv_emit_label(&c->e, label);
}
static void coop_branch(Ctx* c, u32 label) {
    coop_flow(c);
    spv_emit_branch(&c->e, label);
}
static void coop_branch_if(Ctx* c, u32 cond, u32 yes, u32 no) {
    coop_flow(c);
    spv_emit_branch_conditional(&c->e, cond, yes, no);
}
static void coop_merge(Ctx* c, u32 merge) {
    coop_flow(c);
    spv_emit_selection_merge(&c->e, merge);
}
static void coop_switch(Ctx* c, u32 selector, u32 deflt, const u32* literals, const u32* labels, u32 count) {
    coop_flow(c);
    spv_emit_switch_u32(&c->e, selector, deflt, literals, labels, count);
}
static void coop_end(Ctx* c) {
    coop_flow(c);
    spv_emit_return(&c->e);
}
static void coop_demote(Ctx* c) {
    coop_flow(c);
    spv_emit_demote(&c->e);
}
static void note(Ctx* c, u32 variable, u32 write) {
    if (c->notes) shader_note(c->notes, variable, write);
}
static u32 rd(Ctx* c, u32 r) {
    if (r >= SM86_RZ) return c->zero;
    note(c, r, 0);
    coop_acc_take(c, r);
    return spv_emit_load(&c->e, c->t_u32, c->reg[r]);
}
static void wr(Ctx* c, u32 r, u32 v) {
    if (c->cap_on && r == c->cap_reg) { /* a pair's halves are kept, not stored: the select of them writes it */
        c->cap_val = v;
        c->cap_n++;
        return;
    }
    if (r < SM86_RZ) {
        note(c, r, 1);
        coop_acc_take(c, r);
        c->coop_gen[r]++;
        spv_emit_store(&c->e, c->reg[r], v);
    }
}
static u32 urd(Ctx* c, u32 r) {
    if (r >= SM86_URZ) return c->zero;
    note(c, SM86_NOTE_UREG + r, 0);
    return spv_emit_load(&c->e, c->t_u32, c->ureg[r]);
}
static void uwr(Ctx* c, u32 r, u32 v) {
    if (r >= SM86_URZ) return;
    note(c, SM86_NOTE_UREG + r, 1);
    spv_emit_store(&c->e, c->ureg[r], v);
}
static u32 prd(Ctx* c, u32 p, u32 negate, int uniform) {
    if (p < SM86_PT) note(c, (uniform ? SM86_NOTE_UPRED : SM86_NOTE_PRED) + p, 0);
    u32 v = p >= SM86_PT ? c->true_v : spv_emit_load(&c->e, c->t_bool, uniform ? c->upred[p] : c->pred[p]);
    return negate ? spv_emit_unary(&c->e, SPV_OP_LOGICAL_NOT, c->t_bool, v) : v;
}
static void pwr(Ctx* c, u32 p, u32 v, int uniform) {
    if (p >= SM86_PT) return;
    note(c, (uniform ? SM86_NOTE_UPRED : SM86_NOTE_PRED) + p, 1);
    spv_emit_store(&c->e, uniform ? c->upred[p] : c->pred[p], v);
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
static u32 push_component(Ctx* c, u32 k);
static u32 cbuf_window(Ctx* c, u32 bank, u32 element);
static u32 builtin_u32(Ctx* c, u32* var, u32 builtin);
static u32 cbuf_load(Ctx* c, u32 bank, u32 off, u32 index) {
    /* The draw's base instance, which the console's driver writes into bank 0 for a vertex program to add to its
       instance ID (counted from 0, attr_load): BaseInstance less what the draw's first instance adds to the base
       instance (push.x), the GPU's own for an indirect draw; without draw parameters push.x is the base instance. The
       bank an Ounce program reads did not hold it, and a racing game's instanced cactus took other instances'
       transforms (#2175) */
    if (c->stage == ST_VS && bank == 0 && !index && off == SH_CB0_BASE_INSTANCE) {
        u32 push = to_u(c, push_component(c, 0));
        if (!g_spirv_translate.draw_parameters) return push;
        spv_emit_draw_parameters_capability(&c->e);
        return spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32,
                               builtin_u32(c, &c->base_instance, SPV_BUILT_IN_BASE_INSTANCE), push);
    }
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
    if (!root) element = cbuf_window(c, bank, element);
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
/* p + q for two pairs of floats, rounded to odd: where the sum is not exact, the f32 next to it whose last bit is 1,
   toward the exact sum. HFMA2 rounds a * b + c once to f16, and a * b is exact in f32, but the sum rounded to the
   nearest f32 and then by PackHalf2x16 to the nearest f16 can land on an f16 tie that the exact sum is not on (1.5 *
   35040 + 0.0018 is 52560.0018: 52576, not 52544). The odd f32 is never on one, and f32's 13 more bits make its
   nearest f16 the exact sum's. TwoSum's error of the nearest sum is exact, as Vulkan's f32 adds are rounded correctly (a sum
   of f16 values is a multiple of 2^-48, so neither is below the normals); a sum not a number keeps its error from
   moving it */
static u32 sum_to_odd(Ctx* c, u32 p, u32 q) {
    u32 t = c->t_v2f, tu = c->t_v2u, tb = spv_emit_type_vector(&c->e, c->t_bool, 2);
    u32 s = nocontract(c, op2(c, SPV_OP_F_ADD, t, p, q));
    u32 qs = nocontract(c, op2(c, SPV_OP_F_SUB, t, s, p)), ps = nocontract(c, op2(c, SPV_OP_F_SUB, t, s, qs));
    u32 err = nocontract(c, op2(c, SPV_OP_F_ADD, t, nocontract(c, op2(c, SPV_OP_F_SUB, t, p, ps)),
                                nocontract(c, op2(c, SPV_OP_F_SUB, t, q, qs))));
    u32 fzero = spv_emit_constant_vec2(&c->e, t, cf(c, 0.0f), cf(c, 0.0f));
    u32 one = spv_emit_constant_vec2(&c->e, tu, cu(c, 1), cu(c, 1)),
        zero = spv_emit_constant_vec2(&c->e, tu, c->zero, c->zero);
    u32 bits = spv_emit_unary(&c->e, SPV_OP_BITCAST, tu, s);
    u32 even = op2(c, SPV_OP_I_EQUAL, tb, op2(c, SPV_OP_BITWISE_AND, tu, bits, one), zero);
    u32 move = op2(c, SPV_OP_LOGICAL_AND, tb, op2(c, SPV_OP_F_ORD_NOT_EQUAL, tb, err, fzero), even);
    /* away from zero where the error has the sum's sign: one more of the magnitude's bits, else one less */
    u32 away = op2(c, SPV_OP_LOGICAL_EQUAL, tb, op2(c, SPV_OP_F_ORD_LESS_THAN, tb, err, fzero),
                   op2(c, SPV_OP_F_ORD_LESS_THAN, tb, s, fzero));
    u32 step = spv_emit_ternary(&c->e, SPV_OP_SELECT, tu, away, one,
                                spv_emit_constant_vec2(&c->e, tu, cu(c, ~0u), cu(c, ~0u)));
    bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, tu, move, op2(c, SPV_OP_I_ADD, tu, bits, step), bits);
    return spv_emit_unary(&c->e, SPV_OP_BITCAST, t, bits);
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
/* FADD: x + y in mode. A device without doubles (g_spirv_translate.float64) rounds to nearest, as its floats do */
static u32 float_add(Ctx* c, u32 mode, u32 x, u32 y) {
    if (!g_spirv_translate.float64) mode = 0;
    if (mode == 0) return nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, x, y));
    return round_sum(c, mode, widen(c, x), widen(c, y));
}
/* FMUL: x * y * 2^(scale - 4) in mode. The product of two floats is exact in a double */
static u32 float_mul(Ctx* c, u32 mode, u32 scale, u32 x, u32 y) {
    if (!g_spirv_translate.float64) {
        if (scale == 4) return nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, x, y));
        /* 2^(scale - 4), scale 1 to 7. scale is unsigned: below 4, scale - 4 is a number near 2^32, not a
           negative one, so a .D scale divides by 2^(4 - scale) */
        float factor = scale > 4 ? (float)(1u << (scale - 4)) : 1.0f / (float)(1u << (4 - scale));
        /* the power of two goes on the operand it leaves exact, the larger one for a .D and the smaller for a .M, and
           the one product then rounds as the double's does. On the product it rounds twice where x * y is past a
           float's range or subnormal: 2^100 * 2^28 .D4 is 2^126, not infinity */
        u32 ax = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, x);
        u32 ay = spv_emit_glsl_unary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_ABS, y);
        u32 x_scaled =
            op2(c, scale < 4 ? SPV_OP_F_ORD_GREATER_THAN_EQUAL : SPV_OP_F_ORD_LESS_THAN_EQUAL, c->t_bool, ax, ay);
        u32 scaled = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, x_scaled, x, y);
        u32 other = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, x_scaled, y, x);
        scaled = nocontract(
            c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, scaled, spv_emit_constant_f32(&c->e, c->t_f32, factor)));
        return nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, scaled, other));
    }
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
    if (!g_spirv_translate.float64) mode = 0;
    if (mode == 0) return nocontract(c, spv_emit_glsl_ternary(&c->e, c->t_f32, c->glsl, SPV_GLSL_FMA, x, y, z));
    u32 product = nocontract(c, op2(c, SPV_OP_F_MUL, f64_type(c), widen(c, x), widen(c, y)));
    return round_sum(c, mode, product, widen(c, z));
}

/* ---- a vertex program's push constant ---- */

/* component k of the vec4 at push constant offset 0, which the renderer gives a vertex program as it gives the
   Maxwell one (spirv_translate.c): x the base instance's bits, y not 0 when the depth fix is on */
/* With cb_windows, a vertex or fragment program declares one push block for everything it reads of them: the vec4 at 0
   (push_component), the fragment stage's members at 16, 32 and 96 (scaled_slot), then the window offsets of its
   constant buffers (SH_PUSH_CB_WINDOW, SH_PUSH_CB_WINDOW_FS). A program has one push block at most */
static int cb_windowed(const Ctx* c) {
    return g_spirv_translate.cb_windows && (c->stage == ST_VS || c->stage == ST_FS);
}
/* where the draw's per-draw block (SNX_VKB_DRAW) starts in its window, in 16-byte units: the push constant word 0 that
   the renderer gives a graphics program */
static u32 draw_at(Ctx* c) {
    if (!c->draw_at) {
        c->draw_at = spv_emit_push_vec4(&c->e, c->t_v4u, c->t_u32, 0, 0, &c->draw_at_ptr);
        add_interface(c, c->draw_at);
    }
    return spv_emit_push_component_f32(&c->e, c->draw_at_ptr, c->draw_at, c->zero, c->zero, c->t_u32);
}
/* word k of a graphics stage's per-draw block (SNX_VKB_DRAW) at draw_at: its float bits, as the renderer's pc[] holds
   them (callers that read a u32 word bitcast it with to_u). The program declares the whole window (SNX_DRAW_WINDOW) */
static u32 draw_component(Ctx* c, u32 k) {
    if (!c->draw) {
        c->draw = spv_emit_readonly_vec4_array(&c->e, c->t_v4f, c->t_f32, c->t_u32, (u32)shader_vk_set(c->stage),
                                               SNX_VKB_DRAW, SNX_DRAW_WINDOW / 16, &c->draw_ptr);
        add_interface(c, c->draw);
    }
    return spv_emit_uniform_component(&c->e, c->draw_ptr, c->draw, c->zero,
                                      spv_emit_iadd_u32(&c->e, c->t_u32, draw_at(c), cu(c, k / 4)), cu(c, k & 3),
                                      c->t_f32);
}
/* word k of the draw's pc[]: a graphics stage reads it in its per-draw block. A compute program, and a task program of
   the compute-task path (a compute dispatch), read their own push constants, which the dispatch pushes */
static u32 push_component(Ctx* c, u32 k) {
    if (c->stage != ST_CS && !(c->stage == ST_TS && c->payload_buf)) return draw_component(c, k);
    if (!c->push) {
        c->push = spv_emit_push_vec4(&c->e, c->t_v4f, c->t_f32, 0, 0, &c->push_ptr);
        add_interface(c, c->push);
    }
    return spv_emit_push_component_f32(&c->e, c->push_ptr, c->push, c->zero, cu(c, k), c->t_f32);
}
/* element plus the bank's window offset in 16-byte units (a 16-bit field, two banks a word), with cb_windows */
static u32 cbuf_window(Ctx* c, u32 bank, u32 element) {
    if (!cb_windowed(c) || bank >= 18 || bank >= g_spirv_translate.uniform_cbufs) return element;
    c->si->cb_window = 1;
    const u32 word = bank / 2;
    u32 packed;
    /* a vertex program's window offsets are in its per-draw block, the fragment stage's at its own offsets there */
    packed = to_u(c, draw_component(c, (c->stage == ST_VS ? SH_PUSH_CB_WINDOW : SH_PUSH_CB_WINDOW_FS) / 4 + word));
    if (bank & 1) packed = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, packed, cu(c, 16));
    return spv_emit_iadd_u32(&c->e, c->t_u32, element,
                             spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, packed, cu(c, 0xFFFF)));
}
/* The depth fix of spirv_translate.c's vertex_depth_fix: with the push constant's y not 0, z becomes (z + w) / 2,
   from the console's clip space depth of -w to w to Vulkan's of 0 to w */
static void vertex_depth_fix(Ctx* c) {
    u32 enabled = spv_emit_fcompare(&c->e, c->t_bool, 5, push_component(c, 1), cf(c, 0.0f)); /* 5: != */
    u32 adjust = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    coop_merge(c, merge);
    coop_branch_if(c, enabled, adjust, merge);
    coop_label(c, adjust);
    u32 value = spv_emit_load(&c->e, c->t_v4f, c->position);
    u32 sum =
        nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_ADD, c->t_f32, spv_emit_vec4_extract(&c->e, c->t_f32, value, 2),
                                      spv_emit_vec4_extract(&c->e, c->t_f32, value, 3)));
    u32 depth = nocontract(c, spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, sum, cf(c, 0.5f)));
    spv_emit_store(&c->e, c->position, spv_emit_vec4_insert(&c->e, c->t_v4f, value, depth, 2));
    coop_branch(c, merge);
    coop_label(c, merge);
}
static void position_var(Ctx* c) {
    if (!c->position) {
        c->position = spv_emit_position_output(&c->e, c->t_v4f);
        add_interface(c, c->position);
    }
}
/* whether a store to clip distance plane reaches the rasterizer or a later stage: the last stage before rasterization
   writes only the planes the 3D engine enables (SetUserClipEnable), as spirv_translate.c does for Maxwell's */
static int clip_plane_used(const Ctx* c, u32 plane) {
    return (c->si->clip_enable & SNX_SH_NOT_LAST) || (c->si->clip_enable & (1u << plane));
}
/* the ClipDistance output, made before the program's code, so that each plane starts at 0 and a vertex program's
   planes that it doesn't write clip nothing. A geometry program's planes are undefined again after each EmitVertex, as
   on Maxwell's path (spirv_translate.c initializes them once too). It has 8 planes, as spirv_translate.c makes
   Maxwell's, so that the builtin interfaces of adjacent stages agree */
static void clip_var(Ctx* c) {
    if (c->clip) return;
    spv_emit_clip_distance_capability(&c->e);
    c->clip = spv_emit_clip_distance_output(&c->e, c->t_f32, c->t_u32, 8);
    add_interface(c, c->clip);
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
    spv_emit_capability(&c->e, SPV_CAPABILITY_MESH_SHADING_EXT);
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
            spv_emit_decorate(&c->e, c->in_attr[i], SPV_DECORATION_PER_PRIMITIVE_EXT, NULL, 0);
        }
        c->si->in_mask |= 1u << i;
    }
    return c->in_attr[i];
}
/* the bits of component k of generic input i. A fragment program's input that no output of the stage before writes
   (in_unfed_mask) reads 0, 0, 0, 1, as the Maxwell translator reads it, with no Input for the stage before to match */
static u32 input_load(Ctx* c, u32 i, u32 k) {
    if (c->stage == ST_VS && c->si->vertex_fetch) return c->fetched_mask >> i & 1 ? c->fetched[i][k] : c->zero;
    if (c->stage == ST_FS && (c->si->in_unfed_mask >> i & 1)) {
        c->si->in_mask |= 1u << i;
        return to_u(c, cf(c, k == 3 ? 1.0f : 0.0f));
    }
    u32 var = input_var(c, i);
    int integer = c->stage == ST_VS && (c->si->attr_int_mask & (1u << i));
    u32 scalar = integer ? (c->si->attr_sint_mask & (1u << i) ? c->t_i32 : c->t_u32) : c->t_f32;
    u32 ptr = spv_emit_interface_scalar_pointer(&c->e, SPV_STORAGE_INPUT, scalar, var, 0, cu(c, k));
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
    u32 bits = to_u(c, draw_component(c, (own ? 24 : 8) + (slot >= 32)));
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
    if (c->stage == ST_VS && addr == 0x2f8) {
        /* The instance ID counts from 0 whatever the draw's base instance (the game's driver gives the program the
           base instance apart, in c[0]), and Vulkan's InstanceIndex counts from firstInstance. Without draw
           parameters the push constant's x is the base instance. With them it is what the draw's first instance adds
           to the base instance (render.c), and BaseInstance gives the rest, an indirect draw's too, as the Maxwell
           translator has it (emit_io.c). Less the push constant alone, a direct draw's instances counted from its
           base instance there: one game's instanced buildings and trees took other instances' transforms */
        u32 base = to_u(c, push_component(c, 0));
        if (g_spirv_translate.draw_parameters) {
            spv_emit_draw_parameters_capability(&c->e);
            base = spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32,
                                   builtin_u32(c, &c->base_instance, SPV_BUILT_IN_BASE_INSTANCE), base);
        }
        return spv_emit_binary(&c->e, SPV_OP_I_SUB, c->t_u32,
                               builtin_u32(c, &c->instance_index, SPV_BUILT_IN_INSTANCE_INDEX), base);
    }
    if (c->stage == ST_VS && addr == 0x2fc) return builtin_u32(c, &c->vertex_index, SPV_BUILT_IN_VERTEX_INDEX);
    if (c->stage == ST_FS && addr >= 0x70 && addr < 0x80) return to_u(c, frag_coord(c, (addr >> 2) & 3));
    fail(c, "attribute load 0x%x", addr);
    return c->zero;
}
/* the vertices of a geometry program's input primitive: the renderer's key (pipeline.c), as spirv_translate.c reads
   it */
static u32 gs_vertices(const Ctx* c) { return c->si->rt_int_mask; }
/* a geometry program's input at address addr (the position or a generic) as an array of the input primitive's
   vertices, made on its first use: 0 for another address */
static u32 gs_input_var(Ctx* c, u32 addr) {
    if (addr >= 0x70 && addr < 0x80) {
        if (!c->position_in) {
            c->position_in = spv_emit_interface_variable(&c->e, SPV_STORAGE_INPUT, c->t_v4f, c->t_u32, gs_vertices(c),
                                                         SPV_BUILT_IN_POSITION, 0, 0);
            add_interface(c, c->position_in);
        }
        return c->position_in;
    }
    if (addr < 0x80 || addr >= 0x280) return 0;
    u32 i = (addr - 0x80) >> 4;
    if (!c->in_attr[i]) {
        c->in_attr[i] = spv_emit_interface_variable(&c->e, SPV_STORAGE_INPUT, c->t_v4f, c->t_u32, gs_vertices(c),
                                                    SPV_NOT_BUILTIN, i, 0);
        add_interface(c, c->in_attr[i]);
        c->si->in_mask |= 1u << i;
    }
    return c->in_attr[i];
}
/* ALD's attribute at byte address addr of a geometry program's input vertex: the register value that ISBERD handed
   on, the vertex's index in the input primitive (isbe), kept within the primitive */
static u32 gs_attr_load(Ctx* c, u32 addr, u32 vertex) {
    u32 var = gs_input_var(c, addr);
    if (!var) {
        fail(c, "geometry attribute load 0x%x", addr);
        return c->zero;
    }
    u32 index = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, vertex, cu(c, gs_vertices(c) - 1));
    return to_u(c, spv_emit_load(&c->e, c->t_f32,
                                 spv_emit_interface_scalar_pointer(&c->e, SPV_STORAGE_INPUT, c->t_f32, var, index,
                                                                   cu(c, (addr >> 2) & 3))));
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
    if (c->stage != ST_VS && c->stage != ST_GS) {
        fail(c, "attribute store of stage %d", c->stage);
        return;
    }
    if (c->stage == ST_GS && addr == 0x3a0) {
        /* the viewport mask: the primitive goes to viewport 0 where bit 0 is set, and to none where the mask is 0
           (a culling program's passthrough). The emits test it (gs_emit, gs_passthrough) */
        spv_emit_store(&c->e, c->viewport_mask, bits);
        return;
    }
    if (addr >= 0x2c0 && addr < 0x2e0) {
        /* ClipDistance0 to 7, a word each: the plane is in clip_var's output, made before the code */
        u32 plane = (addr - 0x2c0) >> 2;
        if (c->clip && clip_plane_used(c, plane))
            spv_emit_store(&c->e, spv_emit_output_element_pointer(&c->e, c->t_f32, c->clip, cu(c, plane)),
                           to_f(c, bits));
        return;
    }
    if (addr == 0x5c) {
        /* NAK's NAK_ATTR_VPRS_TABLE_INDEX from SM86, the primitive's shading rate: the default rate, one fragment
           a pixel, is what the translation gives without it. The game's driver writes no table for the index (the
           hardware's reset table), so the rate it selects is not known */
        return;
    } else if (addr == 0x64) {
        /* the layer: BuiltIn Layer, which a geometry program writes with the Geometry capability it has, and another
           stage with ShaderLayer where the device has it (spirv_translate.c's rule): without it the write is dropped,
           as there */
        if (c->stage != ST_GS && !g_spirv_translate.output_layer) return;
        if (!c->layer_out) {
            if (c->stage != ST_GS) spv_emit_layer_capability(&c->e);
            c->layer_out = spv_emit_layer_output(&c->e, c->t_i32);
            add_interface(c, c->layer_out);
        }
        spv_emit_store(&c->e, c->layer_out, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, bits));
        c->si->writes_layer = 1;
        return;
    } else if (addr == 0x6c) {
        /* a geometry program's point size needs GeometryPointSize, which the device may lack: then it is 1, as the
           Maxwell translator leaves it (spirv_translate.c declares the output only with
           g_spirv_translate.stage_point_size) */
        if (c->stage == ST_GS && !g_spirv_translate.stage_point_size) return;
        if (!c->point_size) {
            if (c->stage == ST_GS) spv_emit_capability(&c->e, SPV_CAPABILITY_GEOMETRY_POINT_SIZE);
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
    spv_emit_store(&c->e, spv_emit_interface_scalar_pointer(&c->e, SPV_STORAGE_OUTPUT, c->t_f32, var, 0, cu(c, k)),
                   to_f(c, bits));
}
/* whether a geometry program's primitive goes to viewport 0: bit 0 of the mask it wrote, 1 where it wrote none */
static u32 gs_viewport_on(Ctx* c) {
    u32 bit = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, spv_emit_load(&c->e, c->t_u32, c->viewport_mask),
                              cu(c, 1));
    return spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool, bit, c->zero);
}
/* OUT's emit: the vertex of the outputs written so far, with the depth fix where the program is the last before the
   rasterizer, unless the viewport mask sends the primitive to no viewport */
static void gs_emit(Ctx* c) {
    u32 on = gs_viewport_on(c), then = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    spv_emit_selection_merge(&c->e, merge);
    spv_emit_branch_conditional(&c->e, on, then, merge);
    spv_emit_label(&c->e, then);
    if (c->position && !(c->si->clip_enable & SNX_SH_NOT_LAST)) vertex_depth_fix(c);
    spv_emit_emit_vertex(&c->e);
    spv_emit_branch(&c->e, merge);
    spv_emit_label(&c->e, merge);
}
/* A passthrough program's end, as spirv_translate.c's shader_geometry_passthrough has it: the hardware emits the
   input primitive once when the program ends, each output component that POST_VTG_SHADER_ATTRIB_SKIP_MASK does not
   skip (gp_skip_mask, a bit for each scalar: the position at 28, generic a at 32 + 4 a) taken from the input vertex,
   the others as the program wrote them, and the primitive's number. EmitVertex leaves every output undefined, so each
   output the program wrote is saved first and stored again for each vertex. A viewport mask of 0 drops the primitive:
   one game's culling program writes its surviving triangles to memory and passes them on to its depth prepass so */
static void gs_passthrough(Ctx* c) {
    SpvEmitter* e = &c->e;
    u32 n = gs_vertices(c), count = n == 4 ? 2 : n == 6 ? 3 : n;
    u32 out[33] = {0}, in[33] = {0}, saved[33] = {0}, mask[33] = {0};
    u32 parts[4] = {cf(c, 0.0f), cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
    u32 empty = spv_emit_constant_vec4(e, c->t_v4f, parts);
    for (u32 a = 0; a < 33; a++) {
        u32 attr = a ? 32 + 4 * (a - 1) : 28, addr = a ? 0x80 + 16 * (a - 1) : 0x70;
        mask[a] = (~c->si->gp_skip_mask[attr / 32] >> (attr % 32)) & 15;
        /* a generic the stage before doesn't write (rt_sint_mask) is undefined there and here alike: none of NVN's
           draws sets the skip mask, and its 32 generics went past the device's geometry components */
        if (a && !(c->si->rt_sint_mask >> (a - 1) & 1)) mask[a] = 0;
        int written = a ? c->out_attr[a - 1] != 0 : c->position != 0;
        if (!mask[a] && !written) continue;
        out[a] = output_var(c, addr);
        if (mask[a]) in[a] = gs_input_var(c, addr);
        saved[a] = written ? spv_emit_load(e, c->t_v4f, out[a]) : empty;
    }
    u32 size = c->point_size ? spv_emit_load(e, c->t_f32, c->point_size) : 0;
    u32 layer = c->layer_out ? spv_emit_load(e, c->t_i32, c->layer_out) : 0;
    /* and the clip distances, which EmitVertex leaves undefined too (emit_io.c's saved_clip for Maxwell's) */
    u32 clip_saved[8] = {0};
    for (u32 plane = 0; plane < 8 && c->clip; plane++)
        clip_saved[plane] =
            spv_emit_load(e, c->t_f32, spv_emit_output_element_pointer(e, c->t_f32, c->clip, cu(c, plane)));
    if (!c->primitive_id_in) {
        c->primitive_id_in = spv_emit_builtin_input_u32(e, c->t_u32, SPV_BUILT_IN_PRIMITIVE_ID);
        add_interface(c, c->primitive_id_in);
        c->primitive_id_out = spv_emit_interface_variable(e, SPV_STORAGE_OUTPUT, c->t_u32, c->t_u32, 0,
                                                          SPV_BUILT_IN_PRIMITIVE_ID, 0, 0);
        add_interface(c, c->primitive_id_out);
    }
    u32 id = spv_emit_load(e, c->t_u32, c->primitive_id_in);
    u32 on = gs_viewport_on(c), then = spv_emit_id(e), merge = spv_emit_id(e);
    spv_emit_selection_merge(e, merge);
    spv_emit_branch_conditional(e, on, then, merge);
    spv_emit_label(e, then);
    for (u32 v = 0; v < count; v++) {
        u32 index = cu(c, n == 4 ? v + 1 : n == 6 ? v * 2 : v);
        for (u32 a = 0; a < 33; a++) {
            if (!out[a]) continue;
            u32 value = saved[a];
            for (u32 k = 0; k < 4; k++)
                if (mask[a] >> k & 1) {
                    u32 p = spv_emit_interface_scalar_pointer(e, SPV_STORAGE_INPUT, c->t_f32, in[a], index, cu(c, k));
                    value = spv_emit_vec4_insert(e, c->t_v4f, value, spv_emit_load(e, c->t_f32, p), k);
                }
            spv_emit_store(e, out[a], value);
        }
        if (c->point_size) spv_emit_store(e, c->point_size, size);
        if (c->layer_out) spv_emit_store(e, c->layer_out, layer);
        for (u32 plane = 0; plane < 8 && c->clip; plane++)
            spv_emit_store(e, spv_emit_output_element_pointer(e, c->t_f32, c->clip, cu(c, plane)), clip_saved[plane]);
        spv_emit_store(e, c->primitive_id_out, id);
        if (c->position && !(c->si->clip_enable & SNX_SH_NOT_LAST)) vertex_depth_fix(c);
        spv_emit_emit_vertex(e);
    }
    spv_emit_end_primitive(e);
    spv_emit_branch(e, merge);
    spv_emit_label(e, merge);
}

/* ---- global and shared memory ---- */

/* the buffer that names local memory to mem_ptr, mem_load and mem_store */
#define MEM_LOCAL 0xffffffffu
/* ---- global memory by device address (ShCaps.gbuf_address) ---- */

static u32 record_word(Ctx* c, u32 k);
/* A vertex or fragment program of the texture table reads its global buffers by address: the record of the draw holds
   each slot's address and length (SH_RECORD_GBUF), where the renderer bound a descriptor of the same bytes before.
   The word index into the slot (and the atlas's table, SNX_VKB_SMAP) stays as it was */
static int gbuf_by_address(const Ctx* c) {
    return g_spirv_translate.gbuf_address && g_spirv_translate.tex_table && cb_windowed(c);
}
static int gbuf_slot_of(const Ctx* c, u32 buffer) {
    int slot = 0;
    while (slot < 15 && c->gbuf[slot] != buffer) slot++;
    return slot;
}
static int gbuf_is_address(const Ctx* c, u32 buffer) { return c->gbuf_addressed && buffer && buffer != MEM_LOCAL; }
/* a pointer of pointee type to word index of the slot of buffer. Robustness doesn't cover addresses: an index at or past
   the slot's length takes the record's scratch word, where a store is dropped. A load through it reads 0, as a
   descriptor's did (mem_word_load) */
static u32 gbuf_pointer(Ctx* c, u32 buffer, u32 index, u32 type) {
    const u32 rec = 4u * (u32)gbuf_slot_of(c, buffer);
    u32 address = pair64(c, record_word(c, rec), record_word(c, rec + 1));
    u32 words = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, record_word(c, rec + 2), cu(c, 2));
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, index, words);
    address = op2(c, SPV_OP_I_ADD, u64_type(c), address,
                  op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, u64_type(c), to64(c, index), c64(c, 2)));
    u32 scratch = pair64(c, record_word(c, SH_RECORD_SCRATCH), record_word(c, SH_RECORD_SCRATCH + 1));
    address = spv_emit_ternary(&c->e, SPV_OP_SELECT, u64_type(c), inside, address, scratch);
    return spv_emit_address_pointer(&c->e, type, address);
}
/* ---- a vertex program's own attribute fetch (ShCaps.vertex_fetch) ---- */

/* An SM86 vertex program of the record reads its generic attributes from the vertex streams by device address, where
   the draw bound vertex buffers and set a vertex input state before. Attribute i's 4 words in the record
   (SH_RECORD_ATTR + 4 i) give the address of its first element, the bytes from there to its stream's end, and its
   format in the SH_ATTR_ fields (vk/render.c's fetch_plan). The element is the vertex's (VertexIndex holds the draw's
   first vertex and vertex offset), or for an attribute stepped per instance firstInstance + (InstanceIndex -
   firstInstance) / divisor, as Vulkan's fetch has it, BaseInstance being firstInstance */
static int vertex_fetches(const Ctx* c) {
    return g_spirv_translate.vertex_fetch && g_spirv_translate.draw_parameters && c->stage == ST_VS &&
           gbuf_by_address(c);
}
static u32 ubits(Ctx* c, u32 v, u32 offset, u32 count) {
    return spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_U_EXTRACT, c->t_u32, v, cu(c, offset), cu(c, count));
}
static u32 u_equal(Ctx* c, u32 a, u32 b) { return spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, a, b); }
static u32 u_select(Ctx* c, u32 cond, u32 a, u32 b) {
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, cond, a, b);
}
static u32 f_select(Ctx* c, u32 cond, u32 a, u32 b) {
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_f32, cond, a, b);
}
/* the n aligned words from address al on, each 0 where it starts at or past lim, the stream's end: robustness doesn't
   cover addresses, so such a word's load reads the record's scratch word. The word with the stream's last bytes ends
   past lim where the stream ends inside a word, and the renderer binds that word whole (render_draw_main) */
static void fetch_words(Ctx* c, u32 al, u32 lim, u32 scratch, u32 n, u32* w) {
    const u32 t64 = u64_type(c);
    for (u32 j = 0; j < n; j++) {
        u32 at = j ? op2(c, SPV_OP_I_ADD, t64, al, c64(c, 4u * j)) : al;
        u32 inside = op2(c, SPV_OP_U_LESS_THAN, c->t_bool, at, lim);
        u32 pointer =
            spv_emit_address_pointer(&c->e, c->t_u32, spv_emit_ternary(&c->e, SPV_OP_SELECT, t64, inside, at, scratch));
        w[j] = u_select(c, inside, spv_emit_load_aligned(&c->e, c->t_u32, pointer, 4, 0), c->zero);
    }
}
/* the first m words of the attribute's bytes, which start sh bits into w[0] (sh 0, 8, 16 or 24): w[j] >> sh, with
   the low bits of w[j + 1] above. (x << 1) << (31 - sh) is x << (32 - sh), and defined at sh 0, where it is 0 */
static void fetch_align(Ctx* c, const u32* w, u32 n, u32 sh, u32 m, u32* aw) {
    for (u32 j = 0; j < m; j++) {
        aw[j] = op2(c, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, w[j], sh);
        if (j + 1 >= n) continue;
        u32 high =
            op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, w[j + 1], cu(c, 1)),
                op2(c, SPV_OP_I_SUB, c->t_u32, cu(c, 31), sh));
        aw[j] = op2(c, SPV_OP_BITWISE_OR, c->t_u32, aw[j], high);
    }
}
/* an unsigned float of 5 exponent bits over mb mantissa bits (B10G11R11's components) as f32 bits: a denormal is
   m 2^(-14 - mb), exponent 31 an infinity or a NaN */
static u32 small_float(Ctx* c, u32 v, u32 mb) {
    u32 m = ubits(c, v, 0, mb), e = ubits(c, v, mb, 5);
    u32 mantissa = op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, m, cu(c, 23 - mb));
    u32 normal =
        op2(c, SPV_OP_BITWISE_OR, c->t_u32,
            op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, spv_emit_iadd_u32(&c->e, c->t_u32, e, cu(c, 112)), cu(c, 23)),
            mantissa);
    u32 special = op2(c, SPV_OP_BITWISE_OR, c->t_u32, cu(c, 0x7F800000u), mantissa);
    u32 denormal = to_u(c, op2(c, SPV_OP_F_MUL, c->t_f32, spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32, m),
                               cf(c, 1.0f / (float)(1u << (14 + mb)))));
    return u_select(c, u_equal(c, e, c->zero), denormal, u_select(c, u_equal(c, e, cu(c, 31)), special, normal));
}
/* component k of an attribute as the program reads it, from its bits r (zero-extended) of wd bits (0: the format has
   no such component) and the attribute's type: an integer attribute (attr_int_mask) its integer's bits, as its Input
   variable had them, and else a float. A missing component is 0, and 1 as the last */
static u32 fetch_decode(Ctx* c, u32 r, u32 wd, u32 type, u32 k, int integer) {
    u32 s = to_u(c, spv_emit_ternary(&c->e, SPV_OP_BIT_FIELD_S_EXTRACT, c->t_i32,
                                     spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, r), c->zero, wd));
    u32 value = u_select(c, u_equal(c, type, cu(c, 3)), s, r);
    u32 one = cu(c, integer ? 1u : 0x3F800000u);
    if (!integer) {
        /* the largest value of wd bits: (1 << (wd & 31)) - 1 is defined at wd 32 too, where the select takes ~0 */
        u32 most = u_select(c, u_equal(c, wd, cu(c, 32)), cu(c, 0xFFFFFFFFu),
                            op2(c, SPV_OP_I_SUB, c->t_u32,
                                op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, cu(c, 1),
                                    op2(c, SPV_OP_BITWISE_AND, c->t_u32, wd, cu(c, 31))),
                                cu(c, 1)));
        u32 fu = spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32, r);
        u32 fs =
            spv_emit_unary(&c->e, SPV_OP_CONVERT_S_TO_F, c->t_f32, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, s));
        /* the largest value is exactly 1, as Vulkan's fixed-function conversion makes it: OpFDiv may be 2.5 ulp off */
        const u32 smost = op2(c, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, most, cu(c, 1));
        u32 unorm =
            f_select(c, u_equal(c, r, most), cf(c, 1.0f),
                     op2(c, SPV_OP_F_DIV, c->t_f32, fu, spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32, most)));
        u32 snorm =
            f_select(c, u_equal(c, s, smost), cf(c, 1.0f),
                     f_select(c, u_equal(c, s, op2(c, SPV_OP_I_SUB, c->t_u32, c->zero, smost)), cf(c, -1.0f),
                              spv_emit_glsl_binary(&c->e, c->t_f32, c->glsl, SPV_GLSL_F_MAX,
                                                   op2(c, SPV_OP_F_DIV, c->t_f32, fs,
                                                       spv_emit_unary(&c->e, SPV_OP_CONVERT_U_TO_F, c->t_f32, smost)),
                                                   cf(c, -1.0f))));
        u32 half = spv_emit_vec4_extract(&c->e, c->t_f32, SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, r), 0);
        /* float: 32 bits as they are, 16 a half, and the 8-bit and packed formats unorm (the renderer gives those
           type 2) */
        u32 flt =
            f_select(c, u_equal(c, wd, cu(c, 32)), to_f(c, r), f_select(c, u_equal(c, wd, cu(c, 16)), half, unorm));
        u32 f = f_select(
            c, u_equal(c, type, cu(c, 1)), snorm,
            f_select(c, u_equal(c, type, cu(c, 2)), unorm,
                     f_select(c, u_equal(c, type, cu(c, 5)), fu, f_select(c, u_equal(c, type, cu(c, 6)), fs, flt))));
        u32 is_int = op2(c, SPV_OP_LOGICAL_OR, c->t_bool, u_equal(c, type, cu(c, 3)), u_equal(c, type, cu(c, 4)));
        value = u_select(c, is_int, value, to_u(c, f));
    }
    return u_select(c, u_equal(c, wd, c->zero), k == 3 ? one : c->zero, value);
}
/* The size codes a vertex program fetches: the attribute's bytes, the bytes of each component (0 for a packed
   format), and its components */
static const struct {
    u8 code, bytes, comp, n;
} g_fetch_sizes[] = {{0x01, 16, 4, 4}, {0x02, 12, 4, 3}, {0x04, 8, 4, 2}, {0x12, 4, 4, 1}, {0x03, 8, 2, 4},
                     {0x05, 6, 2, 3},  {0x0F, 4, 2, 2},  {0x1B, 2, 2, 1}, {0x0A, 4, 1, 4}, {0x13, 3, 1, 3},
                     {0x18, 2, 1, 2},  {0x1D, 1, 1, 1},  {0x30, 4, 0, 4}, {0x31, 4, 0, 3}};
enum { FETCH_SIZES = sizeof g_fetch_sizes / sizeof *g_fetch_sizes };
/* The fetch of each attribute the program reads, in its first block, before its code: what each ALD of a generic
   reads is then a value of the prologue (input_load). Each attribute reads the aligned words that hold its bytes and
   decodes them in a switch on its size code, uniform in the draw: an attribute marked constant takes the default,
   where no case writes a component */
static void vertex_fetch_prologue(Ctx* c) {
    if (!vertex_fetches(c)) return;
    c->si->vertex_fetch = 1;
    u32 mask = 0;
    for (u32 i = 0; i < c->n; i++) {
        const Sm86Insn* in = &c->in[i];
        if (in->op != SM86_ALD) continue;
        const u32 addr = sm86_field(in, 40, 10), comps = sm86_field(in, 74, 2) + 1;
        for (u32 k = 0; k < comps; k++)
            if (addr + 4 * k >= 0x80 && addr + 4 * k < 0x280) mask |= 1u << ((addr + 4 * k - 0x80) >> 4);
    }
    if (!mask) return;
    /* the Function variables the cases write, before the first block's other instructions */
    const u32 ptr = spv_emit_type_function_pointer(&c->e, c->t_u32);
    u32 raw[4], width[4];
    for (u32 k = 0; k < 4; k++) {
        raw[k] = spv_emit_function_variable_init(&c->e, ptr, c->zero);
        width[k] = spv_emit_function_variable_init(&c->e, ptr, c->zero);
    }
    const u32 t64 = u64_type(c);
    u32 scratch = pair64(c, record_word(c, SH_RECORD_SCRATCH), record_word(c, SH_RECORD_SCRATCH + 1));
    u32 vertex = builtin_u32(c, &c->vertex_index, SPV_BUILT_IN_VERTEX_INDEX);
    spv_emit_draw_parameters_capability(&c->e);
    u32 first = builtin_u32(c, &c->base_instance, SPV_BUILT_IN_BASE_INSTANCE);
    u32 since = op2(c, SPV_OP_I_SUB, c->t_u32, builtin_u32(c, &c->instance_index, SPV_BUILT_IN_INSTANCE_INDEX), first);
    for (u32 i = 0; i < 32; i++) {
        if (!(mask >> i & 1)) continue;
        const u32 rec = SH_RECORD_ATTR + 4 * i;
        u32 addr = pair64(c, record_word(c, rec), record_word(c, rec + 1));
        u32 lim = op2(c, SPV_OP_I_ADD, t64, addr, to64(c, record_word(c, rec + 2)));
        u32 info = record_word(c, rec + 3);
        u32 stride = op2(c, SPV_OP_BITWISE_AND, c->t_u32, info, cu(c, SH_ATTR_STRIDE_MASK));
        u32 size = ubits(c, info, SH_ATTR_SIZE_SHIFT, 6), type = ubits(c, info, SH_ATTR_TYPE_SHIFT, 3);
        /* instanced, constant, BGRA */
        const u32 bits[3] = {SH_ATTR_INSTANCED, SH_ATTR_CONSTANT, SH_ATTR_BGRA};
        u32 flag[3];
        for (u32 f = 0; f < 3; f++)
            flag[f] = spv_emit_binary(&c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
                                      op2(c, SPV_OP_BITWISE_AND, c->t_u32, info, cu(c, bits[f])), c->zero);
        u32 divisor = record_word(c, SH_RECORD_DIVISOR + i);
        divisor = u_select(c, u_equal(c, divisor, c->zero), cu(c, 1), divisor);
        u32 element =
            u_select(c, flag[0],
                     spv_emit_iadd_u32(&c->e, c->t_u32, first, op2(c, SPV_OP_U_DIV, c->t_u32, since, divisor)), vertex);
        u32 at = op2(c, SPV_OP_I_ADD, t64, addr, op2(c, SPV_OP_I_MUL, t64, to64(c, element), to64(c, stride)));
        u32 al = op2(c, SPV_OP_BITWISE_AND, t64, at, c64(c, ~(u64)3));
        u32 sh = op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                     op2(c, SPV_OP_BITWISE_AND, c->t_u32, lo32(c, at), cu(c, 3)), cu(c, 3));
        for (u32 k = 0; k < 4; k++) spv_emit_store(&c->e, width[k], c->zero);
        u32 merge = spv_emit_id(&c->e), literals[FETCH_SIZES], labels[FETCH_SIZES];
        for (u32 f = 0; f < FETCH_SIZES; f++) {
            literals[f] = g_fetch_sizes[f].code;
            labels[f] = spv_emit_id(&c->e);
        }
        /* the selector before the merge: OpSelectionMerge must come right before the OpSwitch */
        const u32 selector = u_select(c, flag[1], c->zero, size);
        coop_merge(c, merge);
        coop_switch(c, selector, merge, literals, labels, FETCH_SIZES);
        for (u32 f = 0; f < FETCH_SIZES; f++) {
            const u32 bytes = g_fetch_sizes[f].bytes, comp = g_fetch_sizes[f].comp, code = g_fetch_sizes[f].code;
            const u32 n = (bytes + 6) / 4, m = (bytes + 3) / 4; /* the words that hold the bytes at any sh */
            u32 w[5], aw[4];
            coop_label(c, labels[f]);
            fetch_words(c, al, lim, scratch, n, w);
            fetch_align(c, w, n, sh, m, aw);
            for (u32 k = 0; k < g_fetch_sizes[f].n; k++) {
                u32 r, wd;
                if (code == 0x30) { /* A2B10G10R10: 10 bits each from bit 0, the last 2 */
                    r = ubits(c, aw[0], 10 * k, k < 3 ? 10 : 2);
                    wd = k < 3 ? 10 : 2;
                } else if (code == 0x31) { /* B10G11R11 UFLOAT: 6 and 5 mantissa bits, then 5 */
                    r = small_float(c, ubits(c, aw[0], 11 * k, k < 2 ? 11 : 10), k < 2 ? 6 : 5);
                    wd = 32;
                } else {
                    r = comp == 4 ? aw[k] : ubits(c, aw[k * comp / 4], (k * comp % 4) * 8, comp * 8);
                    wd = comp * 8;
                }
                spv_emit_store(&c->e, raw[k], r);
                spv_emit_store(&c->e, width[k], cu(c, wd));
            }
            coop_branch(c, merge);
        }
        coop_label(c, merge);
        u32 r[4], wd[4];
        for (u32 k = 0; k < 4; k++) {
            r[k] = spv_emit_load(&c->e, c->t_u32, raw[k]);
            wd[k] = spv_emit_load(&c->e, c->t_u32, width[k]);
        }
        /* BGRA8: the first and third components swap */
        const u32 x = r[0];
        r[0] = u_select(c, flag[2], r[2], x);
        r[2] = u_select(c, flag[2], x, r[2]);
        const int integer = c->si->attr_int_mask >> i & 1;
        for (u32 k = 0; k < 4; k++) c->fetched[i][k] = fetch_decode(c, r[k], wd[k], type, k, integer);
    }
    c->fetched_mask = mask;
    c->si->in_mask |= mask;
}
static u32 gbuf_slot(Ctx* c, u32 key, int write) {
    int slot = -1;
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) slot = i;
    if (slot < 0) {
        /* the last slot holds the shared memory where it is in a buffer (SH_SHARED_SPILL) */
        if (c->si->ngbuf >= (c->si->shared_spill ? SH_SHARED_SPILL : 16)) {
            fail(c, c->si->shared_spill ? "more than 15 global buffers, with the shared memory in a buffer"
                                        : "more than 16 global buffers");
            return 0;
        }
        slot = c->si->ngbuf++;
        c->si->gbuf_cbuf_off[slot] = key;
    }
    if (!c->gbuf[slot] && gbuf_by_address(c)) {
        c->gbuf_addressed = 1;
        c->si->gbuf_address = 1;
        c->gbuf[slot] = spv_emit_id(&c->e); /* a name: gbuf_pointer reads the slot's address from the record */
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
        spv_emit_decorate(&c->e, c->gbuf[slot], SPV_DECORATION_ALIASED, NULL, 0);
        if (!c->coop) spv_emit_decorate(&c->e, c->gbuf[slot], SPV_DECORATION_COHERENT, NULL, 0);
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
static u32 gbuf_bytes(Ctx* c, u32 buffer);
static u32 mem_word_load(Ctx* c, u32 buffer, u32 ptr);
/* the buffer slot of a pointer table's window (SH_GBUF_TABLE), with SNX_VKB_MBASE made: ~0 when it has none */
static u32 table_slot(Ctx* c, u32 key) {
    int slot = -1;
    for (int i = 0; i < c->si->ngbuf; i++)
        if (c->si->gbuf_cbuf_off[i] == key) slot = i;
    if (slot < 0) return ~0u;
    if (!c->mbase) {
        c->mbase = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                               SNX_VKB_MBASE, 16, &c->cbuf_ptr);
        add_interface(c, c->mbase);
    }
    return (u32)slot;
}
static u32 gbuf_base(Ctx* c, u32 key, int wide) {
    u32 parent = SH_GBUF_PARENT(key);
    if (SH_GBUF_BANK(parent) >= 18) { /* the renderer reads the base from a bound bank (vk/render.c) */
        fail(c, "global address whose base is in constant buffer %u", SH_GBUF_BANK(parent));
        return 0;
    }
    if (SH_GBUF_TABLE(key)) { /* the window's base, at its buffer's slot of SNX_VKB_MBASE */
        u32 slot = table_slot(c, key);
        if (slot == ~0u) return 0;
        u32 low = spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mbase, c->zero, cu(c, slot), c->zero, c->t_u32);
        return wide ? pair64(c, low,
                             spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mbase, c->zero, cu(c, slot), cu(c, 1),
                                                        c->t_u32))
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
/* the registers a load of mem_type (NAK's MemType) writes: 64 bits two, 128 four */
static u32 mem_words(u32 type) { return type == 5 ? 2 : type == 6 ? 4 : 1; }
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst);
static void mem_store(Ctx* c, u32 buffer, u32 address, u32 type, u32 data);
static u32 atomic_fadd(Ctx* c, u32 buffer, u32 address, u32 data);
static void global_dynamic(Ctx* c, const Sm86Insn* in, int kind, u32 opcode, int fadd) {
    int op = in->op, ldst_uniform = op == SM86_LDG || op == SM86_STG;
    u32 type = sm86_field(in, 73, 3), words = kind == DYN_ATOMIC ? 1 : mem_words(type);
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
        /* the candidate's own bytes: a window's length at SNX_VKB_MBASE, else the binding's. A draw that binds its
           buffers as one atlas of pages (gbuf_word_index) gives every binding the atlas's array length, and the first
           candidate took every address: its logical words are in SNX_VKB_SMAP */
        u32 length = SH_GBUF_TABLE(keys[k])
                         ? spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mbase, c->zero,
                                                      cu(c, table_slot(c, keys[k])), cu(c, 2), c->t_u32)
                         : gbuf_bytes(c, buffer);
        u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, relative, to64(c, length));
        u32 then = spv_emit_id(&c->e), other = spv_emit_id(&c->e);
        merges[k] = spv_emit_id(&c->e);
        coop_merge(c, merges[k]);
        coop_branch_if(c, inside, then, other);
        coop_label(c, then);
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
        coop_branch(c, merges[k]);
        coop_label(c, other);
    }
    for (u32 k = nkeys; k-- > 0;) {
        coop_branch(c, merges[k]);
        coop_label(c, merges[k]);
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
/* a register's table (TrackState's rtab): a bank's + 1 (SH_GBUF_TABLE), or TAB_SSBO + a storage buffer's index in
   table_ssbo (SH_GBUF_TABLE_SSBO), made on its first use: 0 past the fourth */
enum { TAB_SSBO = 33 };
static u32 table_ssbo_id(Ctx* c, u32 key) {
    for (u32 i = 0; i < c->ntable_ssbo; i++)
        if (c->table_ssbo[i] == key) return TAB_SSBO + i;
    if (c->ntable_ssbo == 4) return 0;
    c->table_ssbo[c->ntable_ssbo] = key;
    return TAB_SSBO + c->ntable_ssbo++;
}
/* where two keys meet (a predicated write over the register's key, or two paths): an indirect key against another
   key takes neither, and the address is then compared with each at run time (global_dynamic), which the two are
   candidates of. Two constant buffer keys keep track_globals's own rules, but where two paths meet and a value from
   memory reaches the register on either side the access is compared at run time anyway, and the two are its
   candidates too (track_candidates) */
static void track_conflict(Ctx* c, u32 a, u32 b) {
    for (u32 k = 0; k < 2; k++) {
        u32 key = k ? b : a, seen = 0;
        if (!key) continue;
        for (u32 j = 0; j < c->ndyn_keys; j++) seen |= c->dyn_keys[j] == key - 1;
        if (!seen && c->ndyn_keys < 16) c->dyn_keys[c->ndyn_keys++] = key - 1;
    }
}
/* the keys of two constant buffer words whose paths meet where a value from memory reaches the register:
   global_dynamic's candidates, each that can hold a buffer's address (a storage buffer descriptor of cbuf 0, or a bare
   16-byte aligned address in a program's own cbuf, as shader_gbuf_tag and shader_gbuf_tag_any take them). Any other
   word an add took as a base takes no buffer slot */
static void track_candidates(Ctx* c, u32 a, u32 b) {
    for (u32 k = 0; k < 2; k++) {
        u32 key = k ? b : a;
        if (!key) continue;
        u32 bank = SH_GBUF_BANK(key - 1), off = SH_GBUF_OFFSET(key - 1);
        if ((off & 15) || bank >= 18 || (bank == 0 && !shader_gbuf_sized(c->stage, off))) continue;
        track_conflict(c, key, 0);
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
        /* the address is one of the table's words itself, plus the offset: the pairs an LDG.64 or .128 loads there
           are its target's cells, and a storage buffer table's pointer reaches the offset's bytes */
        int table_word = ur < SM86_URZ ? reg >= SM86_RZ && s->utabx[ur] : reg < SM86_RZ && s->rtabx[reg];
        if (tab >= TAB_SSBO) {
            /* the bytes the access reaches past the table's pointer: at its immediate offset where the address is
               the pointer itself, an LDG's, STG's or RED's size (bits 73-75: 8, 16, 32, 64 or 128 bits) or an
               atomic's 8 bytes at most */
            u32 type = sm86_field(in, 73, 3), imm = sm86_field(in, 40, 24);
            u32 bytes = op == SM86_ATOMG || op == SM86_ATOMG_NOU || op == SM86_RED ? 8
                        : type < 2                                               ? 1
                        : type < 4                                               ? 2
                        : type == 4                                              ? 4
                        : type == 5                                              ? 8
                                                                                 : 16;
            c->table_ssbo_used |= 1u << (tab - TAB_SSBO);
            if (table_word && !(imm & 0x800000u) && imm + bytes > c->table_reach) c->table_reach = imm + bytes;
            if (!table_word || (imm & 0x800000u)) c->table_reach_open = 1;
        } else if (tab)
            c->table_banks |= 1u << (tab - 1);
        /* an address loaded from the memory a table's word points to: the renderer binds where those cells point
           too (ShaderInfo.table_cells) */
        c->table_cells |= (ur < SM86_URZ ? s->ucell[ur] : 0) | (reg < SM86_RZ ? s->rcell[reg] : 0);
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
            /* An LDG.64 of an address made from a storage buffer's base with an offset the program computes, in a
               graphics program but a fragment one: each pair is a pointer from the buffer's table
               (SH_GBUF_TABLE_SSBO), whose windows the addresses made from it are compared with */
            u32 ssbo = 0;
            int graphics = c->stage == ST_VS || c->stage == ST_TCS || c->stage == ST_TES || c->stage == ST_GS;
            if (!cell && graphics && words >= 2 && (op == SM86_LDG || op == SM86_LDG_NOU) && k && !exact &&
                !key_indirect(k) && !SH_GBUF_TABLE(k - 1) && SH_GBUF_PARENT(k - 1) == k - 1 && !(imm & 7))
                ssbo = table_ssbo_id(c, k - 1);
            int cells = !ssbo && tab < TAB_SSBO && table_word && words >= 2 &&
                        (op == SM86_LDG || op == SM86_LDG_NOU) && !(imm & 7) && imm + 8 * (words / 2 - 1) < 8 * 32;
            for (u32 w = 0; w < words && in->dst + w < SM86_RZ; w++) {
                u32 key = cell && !(w & 1) ? (cell + (w / 2 << 21)) + 1 : 0;
                track_write(c, s, 0, in->dst + w, key, key != 0, !key, (u16)op, always);
                s->rtab[in->dst + w] = ssbo ? (u8)ssbo : cells ? (u8)tab : 0;
                s->rtabx[in->dst + w] = ssbo && !(w & 1);
                s->rcell[in->dst + w] = cells ? 1u << (imm + 4 * (w & ~1u)) / 8 : 0;
            }
        }
        return;
    }
    if (op == SM86_LDS || op == SM86_LDL) { /* data from memory, as a load from global memory's */
        u32 type = sm86_field(in, 73, 3), words = mem_words(type);
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
            } else if (dyn[r] || dyn2[r])
                track_candidates(c, key[r], key2[r]);
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
    if (ok && (c->table_banks || c->table_ssbo_used)) {
        c->si->table_cells = c->table_cells;
        c->si->table_reach = c->table_reach_open ? 0 : c->table_reach;
        u32 others[16], nothers = 0,
                        ntables = snx_popcount64(c->table_banks) + snx_popcount64(c->table_ssbo_used);
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
        for (u32 t = 0; t < c->ntable_ssbo; t++)
            for (u32 w = 0; (c->table_ssbo_used >> t & 1) && w < windows && c->ndyn_keys < 16; w++)
                c->dyn_keys[c->ndyn_keys++] = SH_GBUF_TABLE_SSBO_KEY(c->table_ssbo[t], w);
    }
    free(s);
    free(reached);
    free(out);
    free(block_of);
    free(start);
    return ok;
}
/* The shared memory: the workgroup's array, or where it is in a buffer (ShaderInfo.shared_spill), global buffer slot
   SH_SHARED_SPILL, whose barriers and atomics then order that buffer (UniformMemory) */
static u32 shared_var(Ctx* c) {
    if (!c->shared) {
        u32 bytes = c->si->shared_size > 0 ? (u32)c->si->shared_size : 16384u;
        c->shared_words = (bytes + 3) / 4;
        if (c->si->shared_spill) {
            u32 structure = spv_emit_type_struct_one(&c->e, spv_emit_type_runtime_array(&c->e, c->t_u32, 4));
            c->shared_ptr = spv_emit_type_storage_pointer(&c->e, c->t_u32);
            c->shared =
                spv_emit_storage_buffer(&c->e, structure, (u32)shader_vk_set(c->stage), SNX_VKB_SSBO + SH_SHARED_SPILL);
            spv_emit_decorate(&c->e, c->shared, SPV_DECORATION_COHERENT, NULL, 0);
            c->e.shared_semantics = SPV_MEMORY_SEMANTICS_UNIFORM_MEMORY;
        } else {
            u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, c->shared_words);
            c->shared = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
            c->shared_ptr = spv_emit_type_workgroup_pointer(&c->e, c->t_u32);
            /* The narrow LDSM reads its halves through a second, u16-typed workgroup array. Logical SPIR-V cannot
               bit-cast a Workgroup pointer, so the two views are separate variables and every 32-bit shared store
               also writes its two halves here (shared16_store), as LDSM.T's reads use this view. */
            if (g_spirv_translate.hmma_narrow_lds && !c->narrow_unsafe) {
                if (!c->t_u16) {
                    c->t_u16 = spv_emit_type_int(&c->e, 16, 0);
                    spv_emit_capability(&c->e, SPV_CAPABILITY_INT16);
                }
                c->shared16_words = 2 * c->shared_words;
                u32 array16 = spv_emit_type_array(&c->e, c->t_u16, c->shared16_words);
                c->shared16 = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array16));
                c->shared16_ptr = spv_emit_type_workgroup_pointer(&c->e, c->t_u16);
                add_interface(c, c->shared16);
            }
        }
        add_interface(c, c->shared);
    }
    return c->shared;
}
/* where the shared memory is in a buffer, the first word of this block's part: its place in the grid (x, then y, then
   z) times the program's words */
static u32 shared_block_base(Ctx* c) {
    u32 place = builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, 2);
    for (u32 k = 2; k-- > 0;)
        place = spv_emit_iadd_u32(&c->e, c->t_u32, builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, k),
                                  spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32, place,
                                                  builtin_vec3(c, &c->num_groups, SPV_BUILT_IN_NUM_WORKGROUPS, k)));
    return spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32, place, cu(c, c->shared_words));
}
static u32 smap_word(Ctx* c, u32 index) {
    /* the table's own pointer type: a program of gbuf_address has no global buffer variable to take it from */
    const u32 ptr = spv_emit_type_storage_pointer(&c->e, c->t_u32);
    return spv_emit_load(&c->e, c->t_u32, spv_emit_storage_access_u32(&c->e, ptr, c->smap, c->zero, index));
}
/* Where a word of a global buffer is in what is bound. A draw whose writable storage shares a page with another of its
   buffers has them all bound as one atlas of pages (vk_buf_draw_prepare), and a table (SNX_VKB_SMAP) gives each
   buffer's pages, as shader_gbuf_word_index reads it for Maxwell's programs. One game's culling programs have their
   counters in the last page of their instances every other frame, and read unmapped there, whole objects came and went.
   A dispatch binds each buffer whole, and has no table */
/* the slot of a global buffer's variable, and SNX_VKB_SMAP made (graphics stages only) */
static u32 smap_slot(Ctx* c, u32 buffer) {
    u32 slot = 0;
    while (slot < 15 && c->gbuf[slot] != buffer) slot++;
    if (!c->smap) {
        c->smap = spv_emit_storage_buffer(
            &c->e, spv_emit_type_struct_one(&c->e, spv_emit_type_runtime_array(&c->e, c->t_u32, 4)),
            (u32)shader_vk_set(c->stage), SNX_VKB_SMAP);
        /* NonWritable: no instruction writes the table (the write log, SNX_VKB_WPAGES on the same binding, is a
           compute program's, which has no table), so the shared optimizer (spv_opt_values) reads each word of it
           once where it dominates. Before, each word of global memory loaded it again: a third of the module */
        spv_emit_decorate(&c->e, c->smap, 24, NULL, 0);
        add_interface(c, c->smap);
    }
    return slot;
}
/* The n words from word on (n 1, 2 or 4) in index[]. The table's offset is the buffer's place in its first page of
   guest memory, so at counts the words from the start of a page; the hardware aligns an access of 8 or 16 bytes to
   its size, so its words are in one page, and one lookup of the page gives them all. Each word has its own check of
   the buffer's end */
static void gbuf_word_indices(Ctx* c, u32 buffer, u32 word, u32 n, u32 index[4]) {
    if (c->stage == ST_CS) {
        for (u32 k = 0; k < n; k++) index[k] = k ? spv_emit_iadd_u32(&c->e, c->t_u32, word, cu(c, k)) : word;
        return;
    }
    u32 slot = smap_slot(c, buffer);
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
    for (u32 k = 0; k < n; k++) {
        u32 w = k ? spv_emit_iadd_u32(&c->e, c->t_u32, word, cu(c, k)) : word;
        u32 m = k ? spv_emit_iadd_u32(&c->e, c->t_u32, mapped, cu(c, k)) : mapped;
        m = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, w, count), m, cu(c, ~0u));
        index[k] = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, none, w, m);
    }
}
static u32 gbuf_word_index(Ctx* c, u32 buffer, u32 word) {
    u32 index[4];
    gbuf_word_indices(c, buffer, word, 1, index);
    return index[0];
}
/* A global buffer's bytes as the program sees them: the array's length where it is bound whole, and in an atlas of
   pages its own words that SNX_VKB_SMAP gives, as gbuf_word_index takes them */
static u32 gbuf_bytes(Ctx* c, u32 buffer) {
    u32 bound = gbuf_is_address(c, buffer)
                    ? spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
                                      record_word(c, 4u * (u32)gbuf_slot_of(c, buffer) + 2), cu(c, 2))
                    : SPV(&c->e, SPV_OP_ARRAY_LENGTH, c->t_u32, buffer, 0);
    if (c->stage != ST_CS) {
        u32 slot = smap_slot(c, buffer);
        u32 none = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, smap_word(c, cu(c, slot)), cu(c, ~0u));
        bound = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, none, bound, smap_word(c, cu(c, 32 + slot)));
    }
    return spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, bound, cu(c, 2));
}
/* a word of global (buffer), shared (buffer 0) or local (MEM_LOCAL) memory at the byte address. An address past
   the local memory wraps in it, as spirv_translate.c keeps Maxwell's LDL and STL in theirs */
static u32 mem_ptr(Ctx* c, u32 buffer, u32 address, u32 word) {
    u32 index = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2));
    if (word) index = spv_emit_iadd_u32(&c->e, c->t_u32, index, cu(c, word));
    if (buffer == MEM_LOCAL)
        return spv_emit_function_element_pointer(
            &c->e, c->local_ptr, c->local,
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, index, cu(c, c->local_words - 1)));
    if (gbuf_is_address(c, buffer)) return gbuf_pointer(c, buffer, gbuf_word_index(c, buffer, index), c->t_u32);
    if (buffer)
        return spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, buffer, c->zero, gbuf_word_index(c, buffer, index));
    u32 shared = shared_var(c), last = cu(c, c->shared_words - 1);
    /* the clamp is one UMin, not a compare and a select: ucas proved the two equal over the whole u32 domain */
    index = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, last, index);
    if (c->si->shared_spill)
        return spv_emit_storage_access_u32(&c->e, c->shared_ptr, shared, c->zero,
                                           spv_emit_iadd_u32(&c->e, c->t_u32, shared_block_base(c), index));
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, c->shared_ptr, shared, index);
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
    /* by address: Volatile for what Coherent gave the descriptor's (gbuf_slot), and the alignment it must name. A
       word past the slot's length (gbuf_pointer's scratch word) reads 0, as robustness gave the descriptor's: the
       scratch word holds what the last dropped store left */
    if (gbuf_is_address(c, buffer)) {
        const u32 t64 = u64_type(c);
        u32 scratch = pair64(c, record_word(c, SH_RECORD_SCRATCH), record_word(c, SH_RECORD_SCRATCH + 1));
        u32 past =
            spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, spv_emit_pointer_address(&c->e, t64, ptr), scratch);
        return u_select(c, past, c->zero, spv_emit_load_aligned(&c->e, c->t_u32, ptr, 4, 1));
    }
    return buffer ? spv_emit_load(&c->e, c->t_u32, ptr) : spv_emit_load_nonprivate(&c->e, c->t_u32, ptr);
}
static void mem_word_store(Ctx* c, u32 buffer, u32 ptr, u32 value) {
    if (buffer && buffer != MEM_LOCAL && c->coop)
        spv_emit_atomic_store_u32(&c->e, c->t_u32, ptr, value);
    else if (gbuf_is_address(c, buffer))
        spv_emit_store_aligned(&c->e, ptr, value, 4, 1);
    else if (buffer)
        spv_emit_store(&c->e, ptr, value);
    else
        spv_emit_store_nonprivate(&c->e, ptr, value);
}
/* the n words (1, 2 or 4) of one access at the byte address: a global buffer's through one lookup of their page
   (gbuf_word_indices) */
static void mem_ptrs(Ctx* c, u32 buffer, u32 address, u32 n, u32 ptrs[4]) {
    if (!buffer || buffer == MEM_LOCAL) {
        for (u32 k = 0; k < n; k++) ptrs[k] = mem_ptr(c, buffer, address, k);
        return;
    }
    u32 index[4];
    gbuf_word_indices(c, buffer, spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2)), n,
                      index);
    for (u32 k = 0; k < n; k++)
        ptrs[k] = gbuf_is_address(c, buffer)
                      ? gbuf_pointer(c, buffer, index[k], c->t_u32)
                      : spv_emit_storage_access_u32(&c->e, c->gbuf_ptr, buffer, c->zero, index[k]);
}
/* With hmma_narrow_lds, a 32-bit shared store also writes its two halves to the u16 view that LDSM.T reads. The
   u16 array is a separate workgroup variable, so there is no pointer aliasing in the module. */
static void shared16_store(Ctx* c, u32 address, u32 value) {
    if (!c->shared16) return;
    u32 index = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2));
    u32 base = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, index, cu(c, 1));
    for (u32 k = 0; k < 2; k++) {
        u32 idx = k ? spv_emit_iadd_u32(&c->e, c->t_u32, base, cu(c, 1)) : base;
        idx = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, cu(c, c->shared16_words - 1), idx);
        u32 ptr = spv_emit_function_element_pointer(&c->e, c->shared16_ptr, c->shared16, idx);
        u32 half = spv_emit_int_convert(
            &c->e, c->t_u16,
            spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32,
                            spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, value, cu(c, 16 * k)),
                            cu(c, 0xffff)),
            0);
        spv_emit_store_nonprivate(&c->e, ptr, half);
    }
}
/* a load of mem_type (NAK's MemType) into registers dst on */
static void mem_load(Ctx* c, u32 buffer, u32 address, u32 type, u32 dst, int uniform_dst) {
    u32 words = mem_words(type), ptrs[4];
    mem_ptrs(c, buffer, address, dst >= SM86_RZ ? 1 : words, ptrs);
    for (u32 k = 0; k < words; k++) {
        u32 v = mem_word_load(c, buffer, ptrs[k]);
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
    u32 words = mem_words(type), ptrs[4];
    mem_ptrs(c, buffer, address, words, ptrs);
    for (u32 k = 0; k < words; k++) {
        u32 v = rd(c, data >= SM86_RZ ? data : data + k);
        mem_word_store(c, buffer, ptrs[k], v);
        if (!buffer && g_spirv_translate.hmma_narrow_lds)
            shared16_store(c, spv_emit_iadd_u32(&c->e, c->t_u32, address, cu(c, 4 * k)), v);
    }
}

/* RED.ADD.F32 and ATOMG.ADD.F32 (NAK's AtomType F32, .FTZ.RN): a float atomic add to the word of a global buffer at
   the byte address, the word before it returned. With the device's shaderBufferFloat32AtomicAdd one OpAtomicFAddEXT
   through the buffer's view as floats (the same binding, Aliased as the words'). Else a loop of compare and exchange
   on the word's bits, which ends once no other invocation stored to the word between its load and its exchange: one
   of the lanes that race on a word succeeds each time round. Its sum flushes subnormals as the instruction does
   (ftz) */
static u32 atomic_fadd(Ctx* c, u32 buffer, u32 address, u32 data) {
    if (g_spirv_translate.float_atomic_add && gbuf_is_address(c, buffer)) {
        spv_emit_extension(&c->e, "SPV_EXT_shader_atomic_float_add");
        spv_emit_capability(&c->e, SPV_CAPABILITY_ATOMIC_FLOAT32_ADD_EXT);
        u32 index =
            gbuf_word_index(c, buffer, spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2)));
        return to_u(c, SPV(&c->e, SPV_OP_ATOMIC_F_ADD_EXT, c->t_f32, gbuf_pointer(c, buffer, index, c->t_f32),
                           cu(c, SPV_SCOPE_DEVICE), cu(c, SPV_MEMORY_SEMANTICS_RELAXED), to_f(c, data)));
    }
    if (g_spirv_translate.float_atomic_add) {
        u32 slot = 0;
        while (slot < 15 && c->gbuf[slot] != buffer) slot++;
        if (!c->gbuf_f32[slot]) {
            u32 structure = spv_emit_type_struct_one(&c->e, spv_emit_type_runtime_array(&c->e, c->t_f32, 4));
            c->gbuf_f32[slot] =
                spv_emit_storage_buffer(&c->e, structure, (u32)shader_vk_set(c->stage), SNX_VKB_SSBO + slot);
            spv_emit_decorate(&c->e, c->gbuf_f32[slot], SPV_DECORATION_ALIASED, NULL, 0);
            if (!c->coop) spv_emit_decorate(&c->e, c->gbuf_f32[slot], SPV_DECORATION_COHERENT, NULL, 0);
            add_interface(c, c->gbuf_f32[slot]);
        }
        spv_emit_extension(&c->e, "SPV_EXT_shader_atomic_float_add");
        spv_emit_capability(&c->e, SPV_CAPABILITY_ATOMIC_FLOAT32_ADD_EXT);
        u32 index =
            gbuf_word_index(c, buffer, spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 2)));
        u32 ptr = spv_emit_storage_access_u32(&c->e, spv_emit_type_storage_pointer(&c->e, c->t_f32), c->gbuf_f32[slot],
                                              c->zero, index);
        return to_u(c, SPV(&c->e, SPV_OP_ATOMIC_F_ADD_EXT, c->t_f32, ptr, cu(c, SPV_SCOPE_DEVICE),
                           cu(c, SPV_MEMORY_SEMANTICS_RELAXED), to_f(c, data)));
    }
    SpvEmitter* e = &c->e;
    u32 ptr = mem_ptr(c, buffer, address, 0), header = spv_emit_id(e), body = spv_emit_id(e), cont = spv_emit_id(e),
        merge = spv_emit_id(e);
    spv_emit_store(e, c->cas_var, mem_word_load(c, buffer, ptr));
    coop_branch(c, header);
    coop_label(c, header);
    spv_emit_loop_merge(e, merge, cont);
    coop_branch(c, body);
    coop_label(c, body);
    u32 old = spv_emit_load(e, c->t_u32, c->cas_var);
    u32 sum =
        nocontract(c, spv_emit_binary(e, SPV_OP_F_ADD, c->t_f32, ftz(c, to_f(c, old), 1), ftz(c, to_f(c, data), 1)));
    u32 seen = spv_emit_atomic_compare_exchange_u32(e, c->t_u32, ptr, to_u(c, ftz(c, sum, 1)), old);
    spv_emit_store(e, c->cas_var, seen);
    coop_branch_if(c, spv_emit_binary(e, SPV_OP_I_EQUAL, c->t_bool, seen, old), merge, cont);
    coop_label(c, cont);
    coop_branch(c, header);
    coop_label(c, merge);
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
    u32 at = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, last, index); /* a clamp of the index, not a compare */
    u32 v = spv_emit_load(&c->e, c->t_u32, isbe_ptr(c, var, storage, at));
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, inside, v, c->zero);
}
/* value into word index of an ISBE array of words: a store past its end goes nowhere */
static void isbe_store(Ctx* c, u32 var, u32 storage, u32 words, u32 index, u32 value) {
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, index, cu(c, words));
    u32 then = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    coop_merge(c, merge);
    coop_branch_if(c, inside, then, merge);
    coop_label(c, then);
    spv_emit_store(&c->e, isbe_ptr(c, var, storage, index), value);
    coop_branch(c, merge);
    coop_label(c, merge);
}
static u32 payload_var(Ctx* c) { return isbe_var(c, &c->payload, SPV_STORAGE_TASK_PAYLOAD, c->payload_words); }
static u32 map_var(Ctx* c) { return isbe_var(c, &c->isbe_map, SPV_STORAGE_WORKGROUP, c->isbe_map_words); }
/* The compute-task path's buffer (sm86.h's sm86_mesh_task_cap): a storage buffer of words that the task dispatch
   writes and the mesh draw reads, a flat array with no fixed size */
static u32 payload_block_var(Ctx* c) {
    if (c->payload_block) return c->payload_block;
    u32 array = spv_emit_type_runtime_array(&c->e, c->t_u32, 4);
    u32 block = spv_emit_type_struct_one(&c->e, array);
    c->payload_block = spv_emit_storage_buffer(&c->e, block, (u32)shader_vk_set(c->stage), SNX_VKB_MESHBUF);
    add_interface(c, c->payload_block);
    return c->payload_block;
}
/* a word of the buffer, at index */
static u32 payload_word_ptr(Ctx* c, u32 index) {
    return spv_emit_storage_access_u32(&c->e, spv_emit_type_pointer(&c->e, SPV_STORAGE_STORAGE_BUFFER, c->t_u32),
                                       payload_block_var(c), c->zero, index);
}
/* the task workgroups the draw's buffer holds, above the flag in the push constant's z (sm86.h) */
static u32 payload_cap(Ctx* c) {
    u32 cap = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, to_u(c, push_component(c, 2)), cu(c, 8));
    return spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MAX, cap, cu(c, 1));
}
/* a word of the vec4 at SNX_VKB_RS, which the draw gives the task and mesh stages: 0 the first workgroup of an
   indirect draw (SH_PUSH_GPU_MESH_FIRST) */
static u32 mesh_param(Ctx* c, u32 k) {
    if (!c->mesh_first) {
        c->mesh_first = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                                    SNX_VKB_RS, 1, &c->cbuf_ptr);
        add_interface(c, c->mesh_first);
    }
    return spv_emit_uniform_component(&c->e, c->cbuf_ptr, c->mesh_first, c->zero, c->zero, cu(c, k), c->t_u32);
}
/* this workgroup's slot in the compute-task path's buffer, clamped: the task dispatch's own workgroup, or the task
   workgroup that launched this mesh workgroup */
static u32 payload_slot_index(Ctx* c, u32 groups) {
    u32 group = builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, c->stage == ST_TS ? 0 : 1);
    return spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, group,
                                spv_emit_iadd_u32(&c->e, c->t_u32, groups, cu(c, 0xffffffffu)));
}
/* the first word of that slot: the record and the counts go first, the payloads after them */
static u32 payload_slot(Ctx* c, u32 groups) {
    u32 slot = payload_slot_index(c, groups);
    return spv_emit_iadd_u32(&c->e, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32, slot, cu(c, c->payload_stride)),
                             spv_emit_iadd_u32(&c->e, c->t_u32, cu(c, 4), groups));
}
/* the groups the task dispatch and its mesh draws share: the task's dispatch size cut to the buffer's slots, which it
   stores at the record's word 1 for the mesh program to read */
static u32 payload_groups(Ctx* c) {
    if (!c->payload_groups) {
        /* a Private variable: the module's body function and its entry point read it, and every invocation of a
           workgroup writes the same value */
        c->payload_groups = spv_emit_variable(&c->e, SPV_STORAGE_PRIVATE, c->t_u32);
        add_interface(c, c->payload_groups);
    }
    return spv_emit_load(&c->e, c->t_u32, c->payload_groups);
}
/* a word of the payload, as isbe_load reads one, and one that ISBEWR writes: the workgroup's, past the slots of the
   task workgroups before it in the compute-task path */
static u32 payload_load(Ctx* c, u32 word) {
    if (!c->payload_buf) return isbe_load(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, c->payload_words, word);
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, word, cu(c, c->payload_words));
    u32 at = spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, cu(c, c->payload_words - 1), word);
    u32 ptr = payload_word_ptr(c, spv_emit_iadd_u32(&c->e, c->t_u32, payload_slot(c, payload_groups(c)), at));
    u32 v = spv_emit_load_nonprivate(&c->e, c->t_u32, ptr);
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, inside, v, c->zero);
}
static void payload_store(Ctx* c, u32 word, u32 value) {
    if (!c->payload_buf) {
        isbe_store(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, c->payload_words, word, value);
        return;
    }
    u32 inside = spv_emit_binary(&c->e, SPV_OP_U_LESS_THAN, c->t_bool, word, cu(c, c->payload_words));
    u32 then = spv_emit_id(&c->e), merge = spv_emit_id(&c->e);
    coop_merge(c, merge);
    coop_branch_if(c, inside, then, merge);
    coop_label(c, then);
    spv_emit_store_nonprivate(&c->e,
                              payload_word_ptr(c, spv_emit_iadd_u32(&c->e, c->t_u32, payload_slot(c, payload_groups(c)), word)),
                              value);
    coop_branch(c, merge);
    coop_label(c, merge);
}
/* the count of the mesh groups that task workgroup g launched, at word 4 + g */
static u32 payload_count_ptr(Ctx* c, u32 group) {
    return payload_word_ptr(c, spv_emit_iadd_u32(&c->e, c->t_u32, cu(c, 4), group));
}
/* word k of the draw's record, at word k: the most groups one task workgroup launched, the task workgroups, 1 and 0 */
static u32 payload_record_ptr(Ctx* c, u32 k) { return payload_word_ptr(c, cu(c, k)); }
/* the groups of this dispatch, for the wrapper to keep: the task's, or the mesh program's the task stored */
static u32 payload_groups_compute(Ctx* c) {
    if (c->stage == ST_TS) {
        u32 num = builtin_vec3(c, &c->num_groups, SPV_BUILT_IN_NUM_WORKGROUPS, 0);
        return spv_emit_glsl_binary(&c->e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, num, payload_cap(c));
    }
    return spv_emit_load_nonprivate(&c->e, c->t_u32, payload_record_ptr(c, 1));
}
/* The workgroup index that ISBERD.ATTR [RZ+0x0] reads: the vertex ID of the hardware's point draw for a task
   program, or a mesh program without one, which counts from the draw's first group (the push constant's x, as bits).
   A mesh program after a task program reads its index in its task's launch */
static u32 workgroup_index(Ctx* c) {
    u32 id = builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, 0);
    if (c->stage == ST_MS && c->si->mesh.has_task) return id;
    /* an indirect draw's first group: the record's, in the uniform buffer at SNX_VKB_RS (SH_PUSH_GPU_MESH_FIRST) */
    u32 flags = to_u(c, push_component(c, 2));
    u32 gpu = spv_emit_binary(
        &c->e, SPV_OP_I_NOT_EQUAL, c->t_bool,
        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, flags, cu(c, SH_PUSH_GPU_MESH_FIRST)), c->zero);
    u32 first = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, gpu, mesh_param(c, 0), to_u(c, push_component(c, 0)));
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
    if (c->stage == ST_GS) {
        /* a geometry program's ISBERD names a vertex of its input primitive for ALD: the register's value as it is,
           the vertex's index (as on Maxwell, nxnative's notes) */
        if (write || imm || size || access || skew || output) {
            fail(c, "%s of a geometry program, not the vertex form", sm86_ops[in->op].name);
            return;
        }
        wr(c, in->dst, fixed ? c->zero : rd(c, base));
        return;
    }
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
        wr(c, in->dst, payload_load(c, word));
        return;
    }
    u32 data = write ? rd(c, sm86_field(in, 32, 8)) : 0;
    if (write && task && attr && !skew && output && b32) {
        payload_store(c, word, data);
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
static u32 brx_targets_scan(Ctx* c, u32 i, u32* targets, u32 max);
static int brx_resolve(Ctx* c);
/* the instructions that jump to each one, build_structure's edges: a BRA's target, a BRX's cases. Returns 0 when it
   can't be made */
static int flow_sources(Ctx* c) {
    if (c->jump_first) return 1;
    if (!brx_resolve(c)) return 0;
    u32 n = c->n, total = 0;
    for (u32 i = 0; i < n; i++) {
        const Sm86Insn* in = &c->in[i];
        if (sm86_never(in)) continue;
        if (in->op == SM86_BRA)
            total++;
        else if (is_brx(in->op))
            total += c->brx_table[i].count;
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
            if (in->op == SM86_BRA) {
                u32 t = sm86_branch_target(in, i);
                if (t >= n) continue;
                if (pass == 0)
                    first[t + 1]++;
                else
                    from[first[t] + fill[t]++] = i;
            } else if (is_brx(in->op)) {
                const BrxTable* table = &c->brx_table[i];
                for (u32 k = 0; k < table->count; k++) {
                    u32 t = table->targets[k];
                    if (t >= n) continue;
                    if (pass == 0)
                        first[t + 1]++;
                    else
                        from[first[t] + fill[t]++] = i;
                }
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
        case SM86_PSETP:
        case SM86_UVOTE:
        case SM86_ISETP:
        case SM86_FSETP:
        case SM86_DSETP:
        case SM86_HSETP2:
        case SM86_R2P:
        case SM86_BMOV_B:
        case SM86_BMOV_BI:
        case SM86_BMOV_BL:
        case SM86_STUB_SET:
        case SM86_STUB_END:
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
        case SM86_TXD_U:
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
        case SM86_LDSM_HMMA: /* the pair's two halves: the HMMA's D and the LDSM's fragments */
            return (in->dst < SM86_RZ && r >= in->dst && r < in->dst + 4u) ||
                   (sm86_fuse_ldsm_dst(in) < SM86_RZ && r >= sm86_fuse_ldsm_dst(in) &&
                    r < sm86_fuse_ldsm_dst(in) + 4u);
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
/* whether a texture instruction's results go two halves a register: .F16 (bit 79, NAK's note on TLD) on a float
   result, which all but TXQ's are */
static int tex_halves(const Sm86Insn* in) {
    return sm86_field(in, 79, 1) && in->op != SM86_TXQ && in->op != SM86_TXQ_B && in->op != SM86_TXQ_U;
}
/* the registers a texture instruction writes (tex_results): one a component of its channel mask, or one a pair of
   them with tex_halves */
static u32 tex_result_count(const Sm86Insn* in) {
    u32 n = snx_popcount64(sm86_field(in, 72, 4));
    return tex_halves(in) ? (n + 1) / 2 : n;
}
/* the register a texture instruction's result k goes to: the first two (one with tex_halves) into dst and on, the
   others into dst1 (bits 64-71) and on, or on from dst when dst1 is RZ */
static u32 tex_result_reg(const Sm86Insn* in, u32 k) {
    u32 d0 = in->dst, d1 = sm86_field(in, 64, 8), first = tex_halves(in) ? 1u : 2u;
    if (d1 < SM86_RZ && k >= first) return d1 + k - first;
    return d0 >= SM86_RZ ? d0 : d0 + k;
}
/* may_write, of the words an instruction really writes where its type tells: an ALU's result is one but for those of
   64 bits, an IPA's one, a load's as its size says, and a texture instruction's those its channel mask selects */
static int writes_word(const Sm86Insn* in, u32 r) {
    if (!may_write(in, r)) return 0;
    switch (in->op) {
        case SM86_IPA: return r == in->dst;
        case SM86_LDC: return r < in->dst + (sm86_field(in, 73, 3) == 5 ? 2u : 1u);
        case SM86_LDG:
        case SM86_LDG_NOU:
        case SM86_LDL: return r < in->dst + mem_words(sm86_field(in, 73, 3));
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
        case SM86_TXD_U:
        case SM86_TXQ_U:
            for (u32 k = 0; k < tex_result_count(in); k++)
                if (tex_result_reg(in, k) == r) return 1;
            return 0;
        default: break;
    }
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

/* ---- the texture table (ShCaps.tex_table) ---- */

static u32 uniform_word(Ctx* c, u32 pointer, u32 buffer, u32 index);
/* the image table of a kind of texture, declared once a kind: all at binding 0 of the table's set */
static u32 table_kind(Ctx* c, u32 type, u32 array, u32 shadow, u32 ms, u32* sampled) {
    const u32 key = type | array << 4 | shadow << 5 | ms << 6;
    for (u32 k = 0; k < c->table_kinds; k++)
        if (c->table_kind_key[k] == key) {
            *sampled = c->table_kind_sampled[k];
            return c->table_kind_var[k];
        }
    if (c->table_kinds == 32) return 0; /* never: 3 types, array, shadow and ms make 24 kinds */
    u32 var = spv_emit_image_table(&c->e, c->t_f32, c->t_u32, g_spirv_translate.table_set, 0, type, (int)array,
                                   (int)shadow, ms ? 1 : 0, SNX_TABLE_IMAGES, sampled);
    add_interface(c, var);
    c->table_kind_key[c->table_kinds] = key;
    c->table_kind_var[c->table_kinds] = var;
    c->table_kind_sampled[c->table_kinds++] = *sampled;
    return var;
}
/* Slot i reads its texture through the table where the program's stage has it: its tex[] is made at each instruction
   that samples it (table_element). 0 where the slot keeps a variable of its own */
static int tex_table_slot(Ctx* c, int i, u32 type, u32 array, u32 shadow, u32 ms) {
    c->tex_images[i] = 0;
    if (!g_spirv_translate.tex_table || !cb_windowed(c)) return 0;
    /* a 1D texture is bound as a 2D one (shader.h) */
    c->tex_images[i] = table_kind(c, type == 0 ? 1 : type, array, shadow, ms ? 1 : 0, &c->tex_sampled[i]);
    if (!c->tex_images[i]) return 0;
    c->si->tex_table |= 1;
    c->tex[i] = 0;
    return 1;
}
/* For this instruction, a table slot's tex[]: its word from the draw's words (the window offset at SH_PUSH_TEX_WORDS,
   x the vertex stage's, y the fragment stage's, in 16-byte units, plus the slot), and the image and the sampler that
   the word names */
/* word k of the stage's record in the draw's words: the window offset at SH_PUSH_TEX_WORDS (x the vertex stage's, y the
   fragment stage's, in 16-byte units), plus k */
static u32 record_word(Ctx* c, u32 k) {
    if (!c->tex_words) {
        c->tex_words = spv_emit_uniform_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, (u32)shader_vk_set(c->stage),
                                                   SNX_VKB_TEXWORDS, 4096, &c->tex_words_ptr);
        add_interface(c, c->tex_words);
    }
    /* a vertex program's word window is in its per-draw block, the fragment stage's at its own offset there */
    u32 base = to_u(c, draw_component(c, (c->stage == ST_VS ? SH_PUSH_TEX_WORDS : SH_PUSH_TEX_WORDS + 4) / 4));
    u32 index = spv_emit_iadd_u32(
        &c->e, c->t_u32, spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, base, cu(c, 2)), cu(c, k));
    return uniform_word(c, c->tex_words_ptr, c->tex_words, index);
}
/* ---- the pool map (ShCaps.pool_map, shader.h's SH_MAP_*) ---- */

/* word index of the draw's map: by its device address where the program reads global memory by address, else in the
   storage buffer at the table set's SNX_TABLE_MAP, from the word the per-draw block names. A map holds every index
   its directories name (map_slots clamps the others to 0), so the reads stay in it */
static u32 map_word(Ctx* c, u32 index) {
    if (gbuf_by_address(c)) {
        const u32 t64 = u64_type(c);
        u32 base = pair64(c, to_u(c, draw_component(c, SH_DRAW_MAP)), to_u(c, draw_component(c, SH_DRAW_MAP + 1)));
        u32 at = op2(c, SPV_OP_I_ADD, t64, base, op2(c, SPV_OP_SHIFT_LEFT_LOGICAL, t64, to64(c, index), c64(c, 2)));
        return spv_emit_load_aligned(&c->e, c->t_u32, spv_emit_address_pointer(&c->e, c->t_u32, at), 4, 0);
    }
    if (!c->map) {
        c->map = spv_emit_readonly_vec4_array(&c->e, c->t_v4u, c->t_u32, c->t_u32, g_spirv_translate.table_set,
                                              SNX_TABLE_MAP, SH_MAP_WINDOW / 16, &c->map_ptr);
        add_interface(c, c->map);
    }
    u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, to_u(c, draw_component(c, SH_DRAW_MAP)), index);
    return spv_emit_uniform_component(&c->e, c->map_ptr, c->map, c->zero,
                                      spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, at, cu(c, 2)),
                                      spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, at, cu(c, 3)), c->t_u32);
}
/* the 16-bit field half (0 low, 1 high) of map word index */
static u32 map_half(Ctx* c, u32 index, u32 half) {
    return ubits(c, map_word(c, index), 16u * half, 16);
}
/* whether the pool map gives slot's view and sampler: a plain slot, its handle one word of a constant buffer */
static int map_slot(const Ctx* c, int slot) {
    const ShTex* t = &c->si->tex[slot];
    return g_spirv_translate.pool_map && !(t->flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY)) && !t->bl2;
}
/* slot's word as the draw's texture words hold one (image slot | sampler slot << SNX_TABLE_SAMPLER_SHIFT), from the
   pool map: the handle the slot's constant buffer word holds, its TIC index (bits 0-19) and its TSC index (20-31),
   each against its pool's limit, then each index's record through its directory and page. A handle past a limit
   takes the null slots (SH_MAP_NULL), and reads index 0, which every map holds */
static u32 map_slots(Ctx* c, int slot) {
    const ShTex* t = &c->si->tex[slot];
    c->si->tex_table |= SH_TEX_TABLE_MAP;
    u32 handle = cbuf_load(c, t->bl_cbuf, t->cbuf_off * 4, 0);
    u32 tic = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, handle, cu(c, 0xFFFFF));
    u32 tsc = ubits(c, handle, 20, 12);
    const u32 limits = to_u(c, draw_component(c, SH_DRAW_MAP_LIMITS)),
              header = to_u(c, draw_component(c, SH_DRAW_MAP_HEADER));
    u32 ok = op2(c, SPV_OP_LOGICAL_AND, c->t_bool,
                 op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, tic,
                     spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, limits, cu(c, 0xFFFFF))),
                 op2(c, SPV_OP_U_LESS_THAN_EQUAL, c->t_bool, tsc, ubits(c, limits, 20, 12)));
    tic = u_select(c, ok, tic, c->zero);
    tsc = u_select(c, ok, tsc, c->zero);
    const u32 ms = (t->flags & 2) != 0, type = t->type == 0 ? 1u : t->type;
    const u32 cls = ms ? (t->array ? SH_MAP_CLASS_MS_ARRAY : SH_MAP_CLASS_MS)
                    : type == 2 ? SH_MAP_CLASS_3D
                    : type == 3 ? (t->array ? SH_MAP_CLASS_CUBE_ARRAY : SH_MAP_CLASS_CUBE)
                                : (t->array ? SH_MAP_CLASS_2D_ARRAY : SH_MAP_CLASS_2D);
    u32 page = map_word(c, spv_emit_iadd_u32(&c->e, c->t_u32,
                                             map_word(c, spv_emit_iadd_u32(&c->e, c->t_u32, header, cu(c, SH_MAP_TIC_DIR))),
                                             spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, tic,
                                                             cu(c, SH_MAP_PAGE_SHIFT))));
    u32 record = spv_emit_iadd_u32(
        &c->e, c->t_u32, page,
        op2(c, SPV_OP_I_MUL, c->t_u32,
                        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, tic, cu(c, (1u << SH_MAP_PAGE_SHIFT) - 1)),
                        cu(c, SH_MAP_TIC_WORDS)));
    u32 image = map_half(c, spv_emit_iadd_u32(&c->e, c->t_u32, record, cu(c, cls / 2)), cls & 1);
    u32 tpage = map_word(c, spv_emit_iadd_u32(&c->e, c->t_u32,
                                              map_word(c, spv_emit_iadd_u32(&c->e, c->t_u32, header, cu(c, SH_MAP_TSC_DIR))),
                                              spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, tsc,
                                                              cu(c, SH_MAP_PAGE_SHIFT))));
    u32 srecord = spv_emit_iadd_u32(
        &c->e, c->t_u32, tpage,
        op2(c, SPV_OP_I_MUL, c->t_u32,
                        spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, tsc, cu(c, (1u << SH_MAP_PAGE_SHIFT) - 1)),
                        cu(c, SH_MAP_TSC_WORDS)));
    u32 sampler = map_half(c, srecord, t->shadow ? 1u : 0u);
    image = u_select(c, ok, image, cu(c, SH_MAP_NULL));
    sampler = u_select(c, ok, sampler, cu(c, SH_MAP_NULL));
    return spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, image,
                           spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, sampler,
                                           cu(c, SNX_TABLE_SAMPLER_SHIFT)));
}
static void table_element(Ctx* c, int slot) {
    if (!c->tex_images[slot]) return;
    if (!c->table_samplers) {
        c->table_samplers = spv_emit_sampler_table(&c->e, c->t_u32, g_spirv_translate.table_set, 1, SNX_TABLE_SAMPLERS);
        add_interface(c, c->table_samplers);
    }
    /* after the attributes in a vertex program that fetches them, else after the global buffers, as render.c's
       stage_set_ writes them: by the stage too, as a ShaderInfo that a vertex program's translation set vertex_fetch on
       keeps it */
    const u32 at = c->stage == ST_VS && c->si->vertex_fetch ? SH_RECORD_TEX_FETCH
                   : g_spirv_translate.gbuf_address         ? SH_RECORD_GBUF
                                                            : 0;
    u32 word = record_word(c, at + (u32)slot);
    /* a plain slot reads the map, but where the draw's bit of the slot (SH_DRAW_MAP_WORDS_VS or _FS) says it takes
       its texture word */
    if (map_slot(c, slot)) {
        const u32 masks = c->stage == ST_VS ? SH_DRAW_MAP_WORDS_VS : SH_DRAW_MAP_WORDS_FS;
        u32 own = ubits(c, to_u(c, draw_component(c, masks + (u32)slot / 32)), (u32)slot % 32, 1);
        word = u_select(c, spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool, own, c->zero), map_slots(c, slot), word);
    }
    /* the image index within the table's array whatever the word holds: an index past an array is undefined, where
       robustness doesn't reach */
    u32 image = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, word, cu(c, SNX_TABLE_IMAGES - 1));
    u32 sampler = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, word, cu(c, SNX_TABLE_SAMPLER_SHIFT));
    c->tex[slot] =
        spv_emit_table_element(&c->e, c->tex_sampled[slot], c->tex_images[slot], image, c->table_samplers, sampler);
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
    if (tex_table_slot(c, i, type, array, shadow, ms)) return i;
    /* a 1D texture is bound as a 2D one (shader.h) */
    c->tex[i] = spv_emit_sampler_image(&c->e, c->t_f32, (u32)shader_vk_tex_set(c->stage), SNX_VKB_TEX + (u32)i,
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
    u32 set = (u32)shader_vk_tex_set(c->stage), dim = type == 0 ? 1 : type;
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
                                                       (int)array, 0, g_spirv_translate.tex_array, &c->tex_sampled[i]);
        add_interface(c, c->tex_array[i]);
        if (shadow) {
            c->tex_depth[i] =
                spv_emit_sampler_image_array(&c->e, c->t_f32, c->t_u32, set, SNX_VKB_TEXDEPTH, dim, (int)array, 1,
                                             g_spirv_translate.tex_array, &c->tex_depth_type[i]);
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
    index = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, limit, index); /* a clamp, not a compare and a select */
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
    u32 mask = sm86_field(in, 72, 4), written = 0;
    if (tex_halves(in)) {
        u32 half[4], n = 0;
        for (u32 k = 0; k < 4; k++)
            if (mask & (1u << k)) half[n++] = spv_emit_vec4_extract(&c->e, scalar, value, k);
        for (u32 p = 0; 2 * p < n; p++) {
            u32 pair =
                spv_emit_vec2_construct(&c->e, c->t_v2f, half[2 * p], 2 * p + 1 < n ? half[2 * p + 1] : cf(c, 0.0f));
            wr(c, tex_result_reg(in, p), pack_half(c, pair));
        }
        return;
    }
    for (u32 k = 0; k < 4; k++) {
        if (!(mask & (1u << k))) continue;
        u32 v = spv_emit_vec4_extract(&c->e, scalar, value, k);
        if (scalar != c->t_u32) v = to_u(c, v);
        wr(c, tex_result_reg(in, written++), v);
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
    table_element(c, slot);
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
    int slot = in->op == SM86_TXD_U ? (int)c->handle[c->cur].slot - 1 /* memory_handles' */
                                    : tex_slot(c, (handle >> 14) & 31, (handle & 0x3fff) * 4, type, array, 0, 0);
    if (slot < 0) return;
    table_element(c, slot);
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
    table_element(c, slot);
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
    int mem = in->op == SM86_TXQ_U || in->op == SM86_TXQ_B;
    int slot = mem ? (int)c->handle[c->cur].slot - 1 : -1; /* memory_handles' */
    /* the level: a TXQ.B's is in the register after its handle's */
    u32 level = sm86_field(in, 24, 8);
    if (in->op == SM86_TXQ_B) level = level + 1 < SM86_RZ ? level + 1 : SM86_RZ;
    for (int i = 0; i < c->si->ntex && slot < 0 && !mem; i++)
        if (c->si->tex[i].bl_cbuf == bank && c->si->tex[i].cbuf_off == off &&
            !(c->si->tex[i].flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY)))
            slot = i;
    /* a TLD of a multisampled 2D texture with the handle, anywhere in the program, names its view */
    u32 ms = 0;
    for (u32 k = 0; k < c->n && slot < 0 && !ms && !mem; k++)
        ms = c->in[k].op == SM86_TLD && sm86_field(&c->in[k], 78, 1) && sm86_field(&c->in[k], 40, 19) == handle;
    if (slot < 0 && !mem) slot = tex_slot(c, bank, off * 4, 1, 0, 0, ms);
    if (slot < 0) return;
    table_element(c, slot);
    const ShTex* t = &c->si->tex[slot];
    ms = t->flags & 2;
    u32 dims = (t->type == 2 ? 3 : 2) + t->array; /* a 1D texture is a 2D one, a cube's size is its face's */
    if (t->type == 3) dims = 2 + t->array;
    spv_emit_capability(&c->e, SPV_CAPABILITY_IMAGE_QUERY);
    /* a multisampled texture has one level, and its size takes none */
    u32 size = spv_emit_image_query_size_lod(
        &c->e, spv_emit_type_vector(&c->e, c->t_u32, dims), c->tex_sampled[slot], c->tex[slot],
        ms ? 0 : spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, rd(c, level)));
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
       renderer reads as Maxwell's SUST dimension. A 1D buffer is a storage texel buffer (SPIR-V's Buffer, 5), which the
       renderer binds at SNX_VKB_IMGBUF as for a Maxwell program */
    *type = dim;
    switch (dim) {
        case 0: *sdim = 0, *array = 0, *coords = 1; return 1;
        case 1: *sdim = 5, *array = 0, *coords = 1; return 1;
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
        if (kind != 1) spv_emit_capability(&c->e, SPV_CAPABILITY_STORAGE_IMAGE_READ_WITHOUT_FORMAT);
        c->img[slot] = spv_emit_storage_image(&c->e, image_scalar(c, kind), (u32)shader_vk_set(c->stage),
                                              (sdim == 5 ? SNX_VKB_IMGBUF : SNX_VKB_IMG) + (u32)slot,
                                              sdim == 0 ? 1 : sdim, (int)array, kind == 1 ? 33 : 0, &c->img_type[slot]);
        add_interface(c, c->img[slot]);
    }
    /* integer coordinates, a 1D image's as a 2D one's, a buffer's one element index */
    u32 a = sm86_field(in, 24, 8), v[4], count = 0, coord;
    if (sdim == 5)
        coord = spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, rd(c, a));
    else {
        for (u32 k = 0; k < coords; k++) {
            if (sdim == 0 && k == 1) v[count++] = c->zero;
            v[count++] = rd(c, a >= SM86_RZ ? a : a + k);
        }
        if (sdim == 0 && coords == 1) v[count++] = c->zero;
        u32 ivec = count == 2 ? spv_emit_type_vec2(&c->e, c->t_i32) : spv_emit_type_vec3(&c->e, c->t_i32);
        u32 uvec = count == 2 ? c->t_v2u : c->t_v3u;
        coord = spv_emit_unary(&c->e, SPV_OP_BITCAST, ivec,
                               count == 2 ? spv_emit_vec2_construct(&c->e, uvec, v[0], v[1])
                                          : spv_emit_vec3_construct(&c->e, uvec, v[0], v[1], v[2]));
    }
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
static u32 native_lane(Ctx* c);

/* ---- the warp's matrix instructions (HMMA, LDSM, MOVM), as the lanes of an invocation's own 32 of a subgroup of 32 or
   64 (NVIDIA's warp: lane l of it is the local invocation index's l % 32), each one's registers spread over the lanes
   as PTX's mma.sync, ldmatrix and movmatrix lay them out. A lane reads another's register with a shuffle */
static u32 warp_lane(Ctx* c) {
    return spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, 31));
}
/* value as lane j (0-31) of the invocation's own 32 holds it. The index's high bits are the address of the lanes'
   own 32 in the subgroup's, which every read of a block takes as one value (warp_base) */
static u32 warp_read(Ctx* c, u32 value, u32 j) {
    if (!c->warp_base)
        c->warp_base = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, system_value(c, 0), cu(c, ~31u));
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
    return SPV(&c->e, SPV_OP_GROUP_NON_UNIFORM_SHUFFLE, c->t_u32, cu(c, SPV_SCOPE_SUBGROUP), value,
               spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, c->warp_base, j));
}
/* value as lane l ^ mask of the invocation's own 32 holds it, for lane l (mask below 32) */
static u32 warp_xor(Ctx* c, u32 value, u32 mask) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
    return SPV(&c->e, SPV_OP_GROUP_NON_UNIFORM_SHUFFLE_XOR, c->t_u32, cu(c, SPV_SCOPE_SUBGROUP), value, cu(c, mask));
}
/* the lane of the invocation's own 32 that a read takes: base with k, one of the 16 B words or a quad lane, which is
   base itself for lane 0 */
static u32 warp_index(Ctx* c, u32 base, u32 k) {
    return k ? spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, base, cu(c, k)) : base;
}
/* 4g, the first lane of the reader's own quad: made once, where a read or staged A needs it */
static u32 quad_base(Ctx* c, u32 lane, u32* g4) {
    if (!*g4) *g4 = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 28));
    return *g4;
}
/* value as lane 4g + p of the invocation's own 32 holds it: a quad broadcast where the stage has quad operations,
   which AMD makes a DPP move where a shuffle goes through LDS. Vulkan's quad is four invocations of adjacent subgroup
   ids, the first a multiple of 4, in every stage that has them; so in a subgroup of 64 too the reader's quad is lanes
   4g to 4g + 3 of its own 32, and quad lane p is lane 4g + p */
static u32 quad_read(Ctx* c, u32 value, u32 lane, u32* g4, u32 p) {
    if (c->stage == ST_CS ? g_spirv_translate.quad_compute : c->stage == ST_FS && g_spirv_translate.quad) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
        spv_emit_subgroup_quad_capability(&c->e);
        return spv_emit_subgroup_quad_u32(&c->e, c->t_u32, value, 0, cu(c, p));
    }
    return warp_read(c, value, warp_index(c, quad_base(c, lane, g4), p));
}
static u32 half_pair(Ctx* c, u32 bits) { return SPV_EXT(&c->e, c->t_v2f, SPV_GLSL_UNPACK_HALF2X16, bits); }
const char* const g_shader_hmma_names[SH_HMMA_WAYS] = {"auto", "gather", "pairs", "staged", "astat", "astat-staged"};
int shader_hmma_rule(int subgroups, int radv_warp32, int shuffle_loops) {
    if (!subgroups) return SH_HMMA_GATHER;
    return shuffle_loops ? SH_HMMA_STAGED : radv_warp32 ? SH_HMMA_PAIRS : SH_HMMA_GATHER;
}
void shader_hmma_take(int way, int shared_max) {
    g_spirv_translate.hmma_pairs = way == SH_HMMA_PAIRS;
    g_spirv_translate.hmma_stage = way == SH_HMMA_STAGED || way == SH_HMMA_ASTAT_STAGED ? shared_max : 0;
    g_spirv_translate.hmma_astat = way == SH_HMMA_ASTAT || way == SH_HMMA_ASTAT_STAGED;
}
void shader_hmma_way(char* out, size_t n) {
    int mixed = g_spirv_translate.mixed_dot && g_spirv_translate.float16,
        halves = !mixed && g_spirv_translate.hmma_f16 && g_spirv_translate.float16;
    const char* how = g_spirv_translate.hmma_astat && !halves
                          ? g_spirv_translate.hmma_stage ? "A-stationary, B through a warp's slot of shared memory"
                                                         : "A-stationary, B by shuffles"
                      : g_spirv_translate.hmma_stage ? "A and B through a warp's slot of shared memory"
                      : g_spirv_translate.hmma_pairs && g_spirv_translate.quad_compute
                          ? "A by quad broadcasts, B shared by pairs of lanes"
                      : g_spirv_translate.hmma_pairs   ? "A by shuffles, B shared by pairs of lanes"
                      : g_spirv_translate.quad_compute ? "A by quad broadcasts, B by shuffles"
                                                       : "A and B by shuffles";
    snprintf(out, n, "%s%s, the products %s",
             g_spirv_translate.hmma_coop
                 ? "the device's cooperative matrices where a program's blocks are whole warps and "
                   "their fragments fit in shared memory, else "
                 : "",
             how,
             mixed    ? "by the mixed dot product into f32"
             : halves ? "summed in half floats (hmma_f16), then in f32"
                      : "unpacked to f32");
}
const char* shader_hmma_f16_unused(void) {
    if (!g_spirv_translate.hmma_f16) return NULL;
    if (!g_spirv_translate.float16) return "the device has no shaderFloat16";
    if (g_spirv_translate.mixed_dot)
        return "the device's mixed dot product (VK_VALVE_shader_mixed_float_dot_product) sums two products an "
               "instruction in f32";
    return NULL;
}
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
    u32 never = spv_emit_binary(&c->e, SPV_OP_I_EQUAL, c->t_bool,
                                builtin_vec3(c, &c->num_groups, SPV_BUILT_IN_NUM_WORKGROUPS, 0), c->zero);
    return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, never, cu(c, group + 1), c->zero);
}
/* HMMA on the device's matrix unit (g_spirv_translate.hmma_coop): an OpCooperativeMatrixMulAddKHR of 16xK f16 A and KxN f16 B
   into a 16xN or 8xN f32 accumulator, in place of hmma's 16 B gathers and 32 dot products, on the shape the device
   offers that HMMA best (coop_shape). SPV_KHR_cooperative_matrix leaves which lane holds which element to the
   driver, with no query, so the fragments go through shared memory: each subgroup has a slot of its own (SubgroupId)
   in an array after the program's, where A is row-major (8 words a row of 16 halves), B column-major (8 words a
   column) and D row-major in f32 (N words a row). Which lane owns which element is the same for each shape: lane
   4(r % 8) + c/2 writes A's row r column c and B's column c row k, at their row-major or column-major word (a word
   holds two halves, so c/2 is the word and c % 2 the half of it), and the same lane plus its row g + 8, 64 words
   on, owns the second 8 rows. D is f32, a word an element: lane 4(r % 8) + c/2 owns rows r and r + 8's column c,
   at 8r + c, or 16r + c and 128 words on for a 16 wide row. ucas proved these identities exhaustively: each
   element of each shape lands in exactly one lane's word, and no two of them share one.
   A 16 wide shape has 16 columns for HMMA's 8: B's columns 8-15 are
   whatever the 64 words after its 8 hold (the start of D's), which only reach D's columns 8-15, never read back. A
   16 wide K pads a .1688's k of 8 with zeros in A's columns and B's rows 8-15, as stale words there could be
   infinities or NaNs, and 0 times those poisons the sums; a 16x8x8 shape splits a .16816's K in two products and an
   8x8x8 its M in two more. The accumulator starts at zero and an f16 D's C is added to the lane's four elements of D
   after the product: staging C as well (4 stores, 8 loads) cost one game's video upscaler half of the gain. An f32 D,
   which stays in its matrix (below), takes a C from the registers staged as its accumulator. A subgroup barrier
   comes before the stores (the last HMMA's loads of the slot), between them and the matrix loads, and between D's
   store and the lanes' loads of it; RADV makes them no instruction, a wave's LDS accesses being in order.
   The products of two f16 are exact in f32, and the device sums them in f32 inside the instruction, rounded as it
   does it (RDNA 3.5's is a few f32 places off an FMA chain even on exact sums, as its v_dot2_f32_f16 is); C then adds
   one rounding, in f32. So both D types keep hmma's bound, that of an f32 sum of the products and C, which an f16 D
   then rounds to. On a Radeon 890M (RDNA 3.5, RADV) that upscaler's 7 HMMA programs went from 46.6 ms of GPU time a
   frame to 38.7 (two of them slower, by 4% and 13%), and its movie from 12.6 frames a second to 14.2: the GPU
   gathers and dot products were most of their instructions, and the matrix unit is idle otherwise.
   A GEMM multiplies the same fragments again, and adds each product into the same accumulator, so a matrix stays
   while the registers it came from stay as they are (hmma_coop's coop_frags and coop_acc): a hit saves its stores,
   its barrier and its matrix loads, and an HMMA whose C registers are the last HMMA's D multiplies into that matrix
   instead of reading its words back. The D words go to their registers only where something else reads or writes
   them, which rd and wr see, or before the block ends (coop_flow), and only the f32 D chains: an f16 D rounds at
   each HMMA, so each of its links is written back as it was before */
#define COOP_B     128u /* the words of a slot: A at 0, B, D */
#define COOP_D     192u
#define COOP_WORDS 448u
/* Whether a program's HMMAs take hmma_coop: a compute program with one, in blocks of whole subgroups of 32 (vk/
   compute.c asks for subgroups of 32 for a program that reads other lanes, full where x is a multiple of 32; every
   cooperative matrix spans the subgroup's 32 lanes), with shaderFloat16 for the halves, a shape to take
   (g_spirv_translate.hmma_coop_shapes), and with room for the slots after the program's shared memory within the device's
   (g_spirv_translate.hmma_coop). It is the module's choice, as the extension asks for the Vulkan memory model */
static int coop_wanted(const Ctx* c) {
    if (!g_spirv_translate.hmma_coop || !g_spirv_translate.hmma_coop_shapes || !g_spirv_translate.float16 || c->stage != ST_CS ||
        c->si->cs_local[0] <= 0 || c->si->cs_local[0] % 32)
        return 0;
    u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
    u32 own = c->si->shared_size > 0 ? (u32)c->si->shared_size : 16384u; /* shared_var's */
    if (own + lanes / 32 * COOP_WORDS * 4 > (u32)g_spirv_translate.hmma_coop) return 0;
    for (u32 i = 0; i < c->n; i++)
        if (c->in[i].op == SM86_HMMA || c->in[i].op == SM86_LDSM_HMMA) return 1;
    return 0;
}
static void coop_barrier(Ctx* c) {
    SPV_VOID(&c->e, SPV_OP_CONTROL_BARRIER, cu(c, SPV_SCOPE_SUBGROUP), cu(c, SPV_SCOPE_SUBGROUP),
             cu(c, spv_emit_memory_semantics(&c->e, SPV_MEMORY_SEMANTICS_ACQUIRE_RELEASE |
                                                        SPV_MEMORY_SEMANTICS_WORKGROUP_MEMORY)));
}
static u32 coop_ptr(Ctx* c, u32 index) {
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, spv_emit_type_workgroup_pointer(&c->e, c->t_u32), c->coop_slots, index);
}
static u32 coop_add(Ctx* c, u32 at, u32 k) { return k ? spv_emit_iadd_u32(&c->e, c->t_u32, at, cu(c, k)) : at; }
/* the shape an HMMA takes on the device's cooperative matrices (g_spirv_translate.hmma_coop_shapes): an exact one first,
   16x8x16 for a .16816 and 16x8x8 for a .1688, then a 16 wide K for either (the .1688's second K half zeros, the
   .16816's spare N columns garbage), then the splits, an 8 wide K (two products) and an 8 wide M (two more) */
enum { COOP_SHAPE_16x16x16, COOP_SHAPE_16x8x16, COOP_SHAPE_16x8x8, COOP_SHAPE_8x8x8 };
static u32 coop_shape(int k16) {
    u32 s = g_spirv_translate.hmma_coop_shapes;
    if (k16) {
        if (s & SH_COOP_16x8x16) return COOP_SHAPE_16x8x16;
        if (s & SH_COOP_16x16x16) return COOP_SHAPE_16x16x16;
        if (s & SH_COOP_16x8x8) return COOP_SHAPE_16x8x8;
        return COOP_SHAPE_8x8x8;
    }
    if (s & SH_COOP_16x8x8) return COOP_SHAPE_16x8x8;
    if (s & SH_COOP_16x8x16) return COOP_SHAPE_16x8x16;
    if (s & SH_COOP_16x16x16) return COOP_SHAPE_16x16x16;
    return COOP_SHAPE_8x8x8;
}
/* the M, N and K of each shape */
static const u32 coop_dim[4][3] = {{16, 16, 16}, {16, 8, 16}, {16, 8, 8}, {8, 8, 8}};
/* the matrices each shape takes its fragments in: one a block of 8 rows where its M is 8 wide, and a 16 wide K in two
   8 wide halves where the HMMA's k is 16. A 16 wide K is one product either way */
static u32 coop_kblocks(int k16, u32 shape) { return k16 && coop_dim[shape][2] == 8 ? 2 : 1; }
static u32 coop_mblocks(u32 shape) { return coop_dim[shape][0] == 8 ? 2 : 1; }
/* the slot of the subgroup's own: the array of COOP_WORDS words after the program's shared memory */
static u32 coop_base(Ctx* c) {
    return spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32,
                           builtin_u32(c, &c->subgroup_id, SPV_BUILT_IN_SUBGROUP_ID), cu(c, COOP_WORDS));
}
/* a lane's words (g = l / 4, t = l % 4): 8g, or 16g where the shape's D rows are 16 words (wide), and t. ucas
   proved the mask form of 8g equal to the two shifts over every lane, and the model prices it one less; the wide
   row's one shift more is the same mask's, so it is made at the same time */
static void coop_lane(Ctx* c, u32* g8, u32* t, int wide) {
    u32 lane = warp_lane(c);
    *g8 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                          spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 28)), cu(c, wide ? 2 : 1));
    *t = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3));
}
/* the lane's first D word in its slot: row g of a 16 wide shape's 16 words a row at 2g8, and of an 8 wide one's 8 at
   g8, then its two columns. Its four words are that one, the next, and both a row of 8 lanes further on */
static u32 coop_d_at(Ctx* c, u32 shape) {
    u32 cols = coop_dim[shape][1], drow, t;
    coop_lane(c, &drow, &t, cols == 16);
    return spv_emit_iadd_u32(
        &c->e, c->t_u32, coop_base(c),
        spv_emit_iadd_u32(&c->e, c->t_u32, drow,
                          spv_emit_iadd_u32(&c->e, c->t_u32,
                                            spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 1)),
                                            cu(c, COOP_D))));
}
/* the lane's four D words: k's, of which the two of its row g and the two of row g + 8 */
static u32 coop_d_word(Ctx* c, u32 shape, u32 at_d, u32 k) {
    return coop_ptr(c, coop_add(c, at_d,
                                (k >> 1) * (coop_dim[shape][1] == 16 ? 128 : 64) + (k & 1)));
}
/* the staged fragment a tuple of registers already has, or -1: the same registers, unchanged, staged as this
   shape. A matrix is a value, so a fragment stays while the words are the same, even where the slot is overwritten
   after it */
static int coop_frag_find(Ctx* c, u32 b, u32 first, u32 n, u32 shape) {
    for (u32 i = 0; i < c->nfrags; i++) {
        const CoopFrag* f = &c->coop_frags[i];
        if (f->b != b || f->first != first || f->n != n || f->shape != shape) continue;
        u32 k = 0;
        while (k < n && f->gen[k] == c->coop_gen[first + k]) k++;
        if (k == n) return (int)i;
    }
    return -1;
}
/* the matrices of a staged fragment kept for this block, whatever moved the slot after them. A full cache first drops
   the fragments whose registers were written since, which no HMMA can find again (coop_gen only counts up within a
   block): a straight-line block stages more than COOP_FRAGS of them, and without this its cache stays full of dead
   entries after the first ones, so every HMMA after that stages both its fragments again */
static void coop_frag_hold(Ctx* c, u32 b, u32 first, u32 n, u32 shape, const u32* mat, u32 nmat) {
    if (c->nfrags == COOP_FRAGS) {
        u32 keep = 0;
        for (u32 i = 0; i < c->nfrags; i++) {
            const CoopFrag* f = &c->coop_frags[i];
            u32 k = 0;
            while (k < f->n && f->gen[k] == c->coop_gen[f->first + k]) k++;
            if (k == f->n) c->coop_frags[keep++] = *f;
        }
        c->nfrags = keep;
        if (keep == COOP_FRAGS) return;
    }
    CoopFrag* f = &c->coop_frags[c->nfrags++];
    f->b = b, f->first = first, f->n = n, f->shape = shape, f->nmat = nmat;
    for (u32 k = 0; k < n; k++) f->gen[k] = c->coop_gen[first + k];
    for (u32 k = 0; k < nmat; k++) f->mat[k] = mat[k];
}
/* the D matrix whose words a register's are not, or -1: an HMMA's result is in a matrix and not in the variables, so
   nothing but that gives their words back */
static int coop_acc_find(Ctx* c, u32 r) {
    for (u32 i = 0; i < c->nacc; i++) {
        const CoopAcc* a = &c->coop_acc[i];
        if (r < a->first || r >= a->first + a->n) continue;
        u32 k = 0;
        while (k < a->n && a->gen[k] == c->coop_gen[a->first + k]) k++;
        if (k == a->n) return (int)i;
    }
    return -1;
}
/* the words of a D matrix to its registers go through the slot's D words, one matrix's room (coop_acc_write) */
static void coop_d_store(Ctx* c, u32 shape, const u32* mat, u32 nmat, u32 base) {
    for (u32 m = 0; m < nmat; m++)
        SPV_VOID(&c->e, SPV_OP_COOPERATIVE_MATRIX_STORE_KHR,
                 coop_ptr(c, coop_add(c, coop_add(c, base, COOP_D), m ? 64 : 0)), mat[m],
                 cu(c, SPV_COOPERATIVE_MATRIX_LAYOUT_ROW_MAJOR_KHR), cu(c, coop_dim[shape][1]),
                 SPV_MEMORY_ACCESS_NON_PRIVATE_POINTER);
}
/* the lane's four words of a shape's D (row g columns 2t and 2t + 1, row g + 8's the same), as f32 */
static void coop_d_read(Ctx* c, u32 shape, u32 at_d, u32 out[4]) {
    for (u32 k = 0; k < 4; k++)
        out[k] = to_f(c, spv_emit_load_nonprivate(&c->e, c->t_u32, coop_d_word(c, shape, at_d, k)));
}
/* a held D's words to its registers, through the slot's D words: stored, then read back by each lane. The barrier
   before the store keeps the last reads of those words (another D's, or a 16 wide B's, which spans their start) from
   it, and the one after it the reads from the store */
static void coop_acc_write(Ctx* c, const CoopAcc* a, u32 base) {
    u32 out[4];
    coop_barrier(c);
    coop_d_store(c, a->shape, a->mat, a->nmat, base);
    coop_barrier(c);
    coop_d_read(c, a->shape, coop_d_at(c, a->shape), out);
    for (u32 k = 0; k < 4; k++) wr(c, a->first + k, to_u(c, out[k]));
}
/* the words of the matrix r's register is one of, before a read or a write of it: an other entry stays held, and its
   words are still the matrix's */
static void coop_acc_take(Ctx* c, u32 r) {
    int i = coop_acc_find(c, r);
    if (i < 0) return;
    CoopAcc a = c->coop_acc[i];
    c->coop_acc[i] = c->coop_acc[c->nacc - 1];
    c->nacc--; /* the writes below must not find it again */
    coop_acc_write(c, &a, coop_base(c));
}
/* an HMMA's f32 result kept as the D matrix of the registers it names, instead of their words: 1 where it is held,
   0 where the block already holds its share of matrices and the words must be written. A held D of the same four
   registers is this one's to replace, and one that shares only some of them goes to its registers first: the others
   keep its words */
static int coop_acc_hold(Ctx* c, u32 d, u32 shape, const u32* mat, u32 nmat) {
    for (u32 k = 0; k < 4; k++) {
        int i = coop_acc_find(c, d + k);
        if (i >= 0 && c->coop_acc[i].first == d)
            c->coop_acc[i] = c->coop_acc[--c->nacc];
        else if (i >= 0)
            coop_acc_take(c, d + k);
    }
    for (u32 k = 0; k < 4; k++) c->coop_gen[d + k]++; /* the words are the matrix's now */
    if (c->nacc == COOP_ACC) return 0;
    CoopAcc* a = &c->coop_acc[c->nacc++];
    a->first = d, a->n = 4, a->shape = shape, a->nmat = nmat;
    for (u32 k = 0; k < 4; k++) a->gen[k] = c->coop_gen[d + k];
    for (u32 k = 0; k < nmat; k++) a->mat[k] = mat[k];
    return 1;
}
/* the block's end, at its branches and labels: D's words back to their registers, one held D after the other, and the
   caches empty. A matrix defined here has no value at a label the block's other arms reach too, nor has warp_read's
   lane base, which the next block that reads another lane makes again */
static void coop_flow(Ctx* c) {
    c->warp_base = 0;
    u32 n = c->nacc;
    c->nacc = c->nfrags = 0; /* the writes below must not find them again */
    if (!n) return;
    u32 base = coop_base(c);
    for (u32 i = 0; i < n; i++) coop_acc_write(c, &c->coop_acc[i], base);
}
static void hmma_coop(Ctx* c, const Sm86Insn* in, int k16, int f32, u32 ra, u32 rb, u32 rc, const u32* ldsm_a) {
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    spv_emit_capability(&c->e, SPV_CAPABILITY_FLOAT16);
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
    spv_emit_capability(&c->e, SPV_CAPABILITY_COOPERATIVE_MATRIX_KHR);
    spv_emit_extension(&c->e, "SPV_KHR_cooperative_matrix");
    u32 t_h = spv_emit_type_float(&c->e, 16);
    /* the shape this HMMA takes: the exact one first, then a 16 wide K (a .1688's second K half zeros, a .16816's
       spare N columns garbage), then the splits. coop_wanted admits no module without a shape */
    u32 shape = coop_shape(k16), rows = coop_dim[shape][0], cols = coop_dim[shape][1], depth = coop_dim[shape][2];
    u32 t_a = spv_emit_type_cooperative_matrix(&c->e, t_h, SPV_SCOPE_SUBGROUP, rows, depth,
                                               SPV_COOPERATIVE_MATRIX_USE_MATRIX_A_KHR);
    u32 t_b = spv_emit_type_cooperative_matrix(&c->e, t_h, SPV_SCOPE_SUBGROUP, depth, cols,
                                               SPV_COOPERATIVE_MATRIX_USE_MATRIX_B_KHR);
    u32 t_acc = spv_emit_type_cooperative_matrix(&c->e, c->t_f32, SPV_SCOPE_SUBGROUP, rows, cols,
                                                 SPV_COOPERATIVE_MATRIX_USE_MATRIX_ACCUMULATOR_KHR);
    if (!c->coop_slots) {
        u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
        u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, lanes / 32 * COOP_WORDS);
        c->coop_slots = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
        add_interface(c, c->coop_slots);
    }
    /* lane l's words (g = l / 4, t = l % 4): A's at 8g + t (+64 row g + 8, +4 columns 8-15), B's at 8g + t (+4 rows
       8-15), D's at 16g + 2t (+1 the next column, +128 row g + 8) in a 16 wide one, 8g + 2t (+1, +64 at row g + 8) in
       an 8 wide one */
    /* the fragments a previous HMMA's matrices stand for, and C's matrix: found before any of their words are read,
       so that a matrix they are still in takes no stores, no barrier and no loads. An HMMA whose C registers are
       another shape's D, or were written since, takes their words from the registers, as before */
    u32 kblocks = coop_kblocks(k16, shape), mblocks = coop_mblocks(shape);
    int fa = ra < SM86_RZ ? coop_frag_find(c, 0, ra, na, shape) : -1;
    int fb = rb < SM86_RZ ? coop_frag_find(c, 1, rb, nb, shape) : -1;
    u32 c_mat[2] = {0, 0};
    int c_chain = 0;
    if (f32 && rc < SM86_RZ) {
        int i = coop_acc_find(c, rc);
        if (i >= 0 && c->coop_acc[i].shape == shape && c->coop_acc[i].first == rc) {
            for (u32 m = 0; m < mblocks; m++) c_mat[m] = c->coop_acc[i].mat[m];
            c_chain = 1;
        } else if (i >= 0)
            coop_acc_take(c, rc); /* the words of another shape's matrix go back to the registers first */
    }
    /* the words the staging writes: a .1688's second K half is zeros where the shape's K is 16, and an 8 wide K reads
       only the first half's, whose .1688 has none to zero */
    u32 ka = k16 || depth == 16 ? 4 : 2, kb = k16 || depth == 16 ? 2 : 1;
    /* C's words, which the product takes from the registers where no matrix stands for them: an f16 D's are added to
       the lane's words after the product, as they were before the matrices stayed (staging them costs a matrix load
       for the 4 word loads and adds it takes), and an f32 D's are staged as the product's accumulator, as that D
       stays in its matrix, where nothing would add them after */
    int stage_c = f32 && rc < SM86_RZ && !c_chain;
    if (rc < SM86_RZ && !c_chain)
        for (k = 0; k < ncd; k++) cc[k] = rd(c, rc + k);
    /* the fragments no matrix stands for, staged in the slot, with the barrier that keeps the last HMMA's loads of it
       away. An RZ fragment's zeros stage too, as the words there can be anything */
    int words = (ra < SM86_RZ ? fa < 0 : 1) || (rb < SM86_RZ ? fb < 0 : 1) || stage_c;
    u32 base = words || fa < 0 || fb < 0 ? coop_base(c) : 0, g8 = 0, t = 0, at_a = 0, at_b = 0;
    if (words) {
        if (fa < 0) /* an RZ fragment's too: zeros */
            for (k = 0; k < na; k++) a[k] = ra >= SM86_RZ ? c->zero : ldsm_a ? ldsm_a[k] : rd(c, ra + k);
        if (fb < 0)
            for (k = 0; k < nb; k++) b[k] = rb >= SM86_RZ ? c->zero : rd(c, rb + k);
        coop_lane(c, &g8, &t, 0);
        at_a = spv_emit_iadd_u32(&c->e, c->t_u32, base, spv_emit_iadd_u32(&c->e, c->t_u32, g8, t));
        at_b = coop_add(c, at_a, COOP_B);
        coop_barrier(c);
        static const u32 a_at[4] = {0, 64, 4, 68};
        /* a .1688's K pad: -0 halves in A's and +0 in B's, whose -0 products leave the sum and its sign of zero */
        if (ra < SM86_RZ ? fa < 0 : 1)
            for (k = 0; k < ka; k++)
                spv_emit_store_nonprivate(&c->e, coop_ptr(c, coop_add(c, at_a, a_at[k])),
                                          k < na ? a[k] : cu(c, 0x80008000u));
        if (rb < SM86_RZ ? fb < 0 : 1)
            for (k = 0; k < kb; k++)
                spv_emit_store_nonprivate(&c->e, coop_ptr(c, coop_add(c, at_b, 4 * k)), k < nb ? b[k] : c->zero);
        if (stage_c) { /* where the lane reads its D words back: B's spare columns of a 16 wide one take them too */
            u32 at_c = coop_d_at(c, shape);
            for (k = 0; k < 4; k++) spv_emit_store_nonprivate(&c->e, coop_d_word(c, shape, at_c, k), cc[k]);
        }
        coop_barrier(c);
    }
    for (u32 m = 0; stage_c && m < mblocks; m++)
        c_mat[m] =
            SPV(&c->e, SPV_OP_COOPERATIVE_MATRIX_LOAD_KHR, t_acc,
                coop_ptr(c, coop_add(c, coop_add(c, base, COOP_D), m ? 64 : 0)),
                cu(c, SPV_COOPERATIVE_MATRIX_LAYOUT_ROW_MAJOR_KHR), cu(c, cols), SPV_MEMORY_ACCESS_NON_PRIVATE_POINTER);
    /* the products: an 8 wide K splits a .16816's K in two, an 8 wide M its 16 rows in two blocks of 8, each block's
       K halves adding into one accumulator. A 16 wide K is one product */
    static const u32 half_at[2] = {0, 4}, block_at[2] = {0, 64};
    /* the sum's start where no C is in it: an RZ C's +0, which the console adds, or -0 where a C is added after, so
       that the sum of -0 products and a -0 C stays -0 */
    u32 zero_acc = spv_emit_const_composite(&c->e, t_acc, (const u32[]){cf(c, rc < SM86_RZ ? -0.0f : 0.0f)}, 1);
    u32 d_mat[2], ma_mat[4], mb_mat[2];
    for (u32 h = 0; h < kblocks; h++) /* B's matrix, one a k half: the same words for every M block */
        if (fb < 0)
            mb_mat[h] = SPV(&c->e, SPV_OP_COOPERATIVE_MATRIX_LOAD_KHR, t_b,
                             coop_ptr(c, coop_add(c, coop_add(c, base, COOP_B), half_at[h])),
                             cu(c, SPV_COOPERATIVE_MATRIX_LAYOUT_COLUMN_MAJOR_KHR), cu(c, 8),
                             SPV_MEMORY_ACCESS_NON_PRIVATE_POINTER);
    for (u32 m = 0; m < mblocks; m++) {
        u32 acc = c_chain || stage_c ? c_mat[m] : 0;
        for (u32 h = 0; h < kblocks; h++) {
            u32 ma = fa >= 0 ? c->coop_frags[fa].mat[m * kblocks + h]
                             : SPV(&c->e, SPV_OP_COOPERATIVE_MATRIX_LOAD_KHR, t_a,
                                   coop_ptr(c, coop_add(c, coop_add(c, base, block_at[m]), half_at[h])),
                                   cu(c, SPV_COOPERATIVE_MATRIX_LAYOUT_ROW_MAJOR_KHR), cu(c, 8),
                                   SPV_MEMORY_ACCESS_NON_PRIVATE_POINTER);
            if (fa < 0) ma_mat[m * kblocks + h] = ma;
            u32 mb = fb >= 0 ? c->coop_frags[fb].mat[h] : mb_mat[h];
            acc = SPV(&c->e, SPV_OP_COOPERATIVE_MATRIX_MUL_ADD_KHR, t_acc, ma, mb, acc ? acc : zero_acc);
        }
        d_mat[m] = acc;
    }
    if (fa < 0 && ra < SM86_RZ) coop_frag_hold(c, 0, ra, na, shape, ma_mat, mblocks * kblocks);
    if (fb < 0 && rb < SM86_RZ) coop_frag_hold(c, 1, rb, nb, shape, mb_mat, kblocks);
    /* D's words: an f32 one stays in the matrix until something reads or writes those registers, and an f16 one is
       written back now, as its halves round at each HMMA */
    u32 d = in->dst;
    if (d >= SM86_RZ) return;
    if (f32 && coop_acc_hold(c, d, shape, d_mat, mblocks)) return;
    /* the barrier this D store needs where the staging above had none: the words of a previous D were read after
       their own, and a matrix load of C came between those and this */
    if (!words) coop_barrier(c);
    u32 at_d = coop_d_at(c, shape);
    coop_d_store(c, shape, d_mat, mblocks, base ? base : coop_base(c));
    coop_barrier(c);
    u32 acc[4];
    coop_d_read(c, shape, at_d, acc);
    if (rc < SM86_RZ && !c_chain && !stage_c) /* C was the product's accumulator where it had a matrix */
        for (u32 i = 0; i < 2; i++) {
            u32 pair = f32 ? 0 : half_pair(c, cc[i]);
            for (u32 j = 0; j < 2; j++) {
                u32 addend = f32 ? to_f(c, cc[2 * i + j]) : spv_emit_vec4_extract(&c->e, c->t_f32, pair, j);
                acc[2 * i + j] = op2(c, SPV_OP_F_ADD, c->t_f32, acc[2 * i + j], addend);
            }
        }
    if (f32)
        for (k = 0; k < 4; k++) wr(c, d + k, to_u(c, acc[k]));
    else
        for (u32 i = 0; i < 2; i++)
            wr(c, d + i, pack_half(c, spv_emit_vec2_construct(&c->e, c->t_v2f, acc[2 * i], acc[2 * i + 1])));
}
/* hmma's A and B through shared memory (g_spirv_translate.hmma_stage), for a device whose shuffles cost far more than a
   store and a load: each warp has a slot of HMMA_STAGE_WORDS after the program's shared memory, lane l's A words at 4l
   and its B words at 128 + 2l, between two subgroup barriers as hmma_coop's slots are. A compute program's, where the
   slots fit the device's shared memory beside the program's own */
#define HMMA_STAGE_WORDS 192u
static int stage_wanted(const Ctx* c) {
    if (!g_spirv_translate.hmma_stage || c->coop || c->stage != ST_CS || c->si->cs_local[0] <= 0) return 0;
    u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
    u32 own = c->si->shared_size > 0 ? (u32)c->si->shared_size : 16384u; /* shared_var's */
    return own + (lanes + 31) / 32 * HMMA_STAGE_WORDS * 4 <= (u32)g_spirv_translate.hmma_stage;
}
static u32 stage_ptr(Ctx* c, u32 index) {
    return SPV(&c->e, SPV_OP_ACCESS_CHAIN, spv_emit_type_workgroup_pointer(&c->e, c->t_u32), c->stage_slots, index);
}
/* HMMA A-stationary (g_spirv_translate.hmma_astat): lane l = 4g + t holds A's pairs of rows g and g + 8 at k words t
   (a0, a1) and t + 4 (a2, a3), and forms every product they take part in: with B's words of the same k words of each
   of the 8 columns, those of lane 4c + t (b0, b1). Its partial sum of row i and column c = j ^ 2t sits in slot j (a
   skew by t, so that the butterfly sends the same slots from every lane): 16 partial sums of 4 products (2 for k8).
   The lanes of a quad hold the same 16 (row, column) pairs, each summed over its own k words, and D's owner of
   (row, c) is the quad lane c >> 1. A butterfly over the quad: at bit b (xor 1, then xor 2) a lane sends the slots
   whose bit b + 1 is set and adds what it receives into slot j ^ (2 << b), which holds the same pair there. Slots 0
   and 1 are then columns 2t and 2t + 1, all of their k words: D = them + C. 16 B words fetched (shuffles, or loads from
   the warp's slot where hmma_stage takes it, B at 128 + 2l as hmma's), 12 xor shuffles, 12 adds: ucas's mma chose it
   under its gt1030 and sm86 models, and proved it over the 32 lanes (each D element exactly its 16 products and C).
   The sums are a tree: a term goes through 6 roundings at most, where hmma's chain has 16 */
static void hmma_astat(Ctx* c, const Sm86Insn* in, int k16, int f32, u32 ra, u32 rb, u32 rc) {
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    for (k = 0; k < na; k++) a[k] = ra >= SM86_RZ ? c->zero : rd(c, ra + k);
    for (k = 0; k < nb; k++) b[k] = rb >= SM86_RZ ? c->zero : rd(c, rb + k);
    for (k = 0; k < ncd; k++) cc[k] = rc >= SM86_RZ ? c->zero : rd(c, rc + k);
    int mixed = g_spirv_translate.mixed_dot && g_spirv_translate.float16;
    u32 t_v2h = 0;
    if (mixed) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_FLOAT16);
        t_v2h = spv_emit_type_vector(&c->e, spv_emit_type_float(&c->e, 16), 2);
        spv_emit_capability(&c->e, SPV_CAPABILITY_DOT_PRODUCT_FLOAT16_ACC_FLOAT32_VALVE);
        spv_emit_extension(&c->e, "SPV_VALVE_mixed_float_dot_product");
    }
    u32 lane = warp_lane(c);
    u32 t = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3));
    /* slot j's column is j ^ 2t, its B words lane 4 (j ^ 2t) + t's: (t ^ 8t) ^ 4j */
    u32 src0 = spv_emit_binary(&c->e, SPV_OP_BITWISE_XOR, c->t_u32, t,
                               spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 3)));
    u32 b_at = 0;
    if (stage_wanted(c)) {
        if (!c->stage_slots) {
            u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
            u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, (lanes + 31) / 32 * HMMA_STAGE_WORDS);
            c->stage_slots = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
            add_interface(c, c->stage_slots);
        }
        u32 slot = spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32,
                                   spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
                                                   builtin_u32(c, &c->local_index, SPV_BUILT_IN_LOCAL_INVOCATION_INDEX),
                                                   cu(c, 5)),
                                   cu(c, HMMA_STAGE_WORDS));
        u32 own_b = spv_emit_iadd_u32(
            &c->e, c->t_u32, slot,
            spv_emit_iadd_u32(&c->e, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, lane, cu(c, 1)), cu(c, 128)));
        coop_barrier(c); /* the HMMA before's loads of the slot */
        for (k = 0; k < nb; k++) spv_emit_store_nonprivate(&c->e, stage_ptr(c, coop_add(c, own_b, k)), b[k]);
        coop_barrier(c);
        b_at = coop_add(c, slot, 128); /* lane L's B words at 128 + 2L */
    }
    u32 ap[4][2] = {{0}}; /* A's pairs, as two f16 (mixed) or unpacked to two f32 */
    for (k = 0; k < na; k++)
        if (mixed)
            ap[k][0] = spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, a[k]);
        else {
            u32 pair = half_pair(c, a[k]);
            ap[k][0] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 0);
            ap[k][1] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 1);
        }
    u32 part[2][8]; /* [row g, g + 8][slot j] */
    for (u32 j = 0; j < 8; j++) {
        u32 bw[2];
        for (k = 0; k < nb; k++) {
            if (b_at) { /* 2 (src0 ^ 4j): the xor, as src0 has bits where 4j does */
                u32 at = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                         spv_emit_binary(&c->e, SPV_OP_BITWISE_XOR, c->t_u32, src0, cu(c, 4 * j)),
                                         cu(c, 1));
                bw[k] = spv_emit_load_nonprivate(&c->e, c->t_u32,
                                                 stage_ptr(c, coop_add(c, spv_emit_iadd_u32(&c->e, c->t_u32, b_at, at), k)));
            } else
                bw[k] = warp_read(c, b[k], spv_emit_binary(&c->e, SPV_OP_BITWISE_XOR, c->t_u32, src0, cu(c, 4 * j)));
        }
        u32 bp[2][2] = {{0}};
        for (k = 0; k < nb; k++)
            if (mixed)
                bp[k][0] = spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, bw[k]);
            else {
                u32 pair = half_pair(c, bw[k]);
                bp[k][0] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 0);
                bp[k][1] = spv_emit_vec4_extract(&c->e, c->t_f32, pair, 1);
            }
        for (u32 i = 0; i < 2; i++) {
            u32 acc = 0;
            for (u32 h = 0; h < nb; h++) { /* A's register 2h + i, B's h: the k words t and t + 4 */
                if (mixed) {
                    acc = SPV(&c->e, SPV_OP_F_DOT2_MIX_ACC32_VALVE, c->t_f32, ap[2 * h + i][0], bp[h][0],
                              acc ? acc : cf(c, 0.0f));
                    continue;
                }
                for (u32 e = 0; e < 2; e++) {
                    u32 prod = op2(c, SPV_OP_F_MUL, c->t_f32, ap[2 * h + i][e], bp[h][e]);
                    acc = acc ? op2(c, SPV_OP_F_ADD, c->t_f32, acc, prod) : prod;
                }
            }
            part[i][j] = acc;
        }
    }
    for (u32 bit = 0; bit < 2; bit++) /* the butterfly over the quad */
        for (u32 j = 0; j < 8; j++) {
            if (!(j >> (bit + 1) & 1) || (bit == 1 && (j & 2))) continue; /* the slots still live that it sends */
            for (u32 i = 0; i < 2; i++) {
                u32 x = to_f(c, warp_xor(c, to_u(c, part[i][j]), 1u << bit));
                u32 to = j ^ (2u << bit);
                part[i][to] = op2(c, SPV_OP_F_ADD, c->t_f32, part[i][to], x);
            }
        }
    u32 acc[4]; /* row g columns 2t and 2t + 1, row g + 8 the same: slots 0 and 1 */
    for (u32 i = 0; i < 2; i++) {
        u32 pair = f32 ? 0 : half_pair(c, cc[i]);
        for (u32 j = 0; j < 2; j++) {
            u32 addend = f32 ? to_f(c, cc[2 * i + j]) : spv_emit_vec4_extract(&c->e, c->t_f32, pair, j);
            acc[2 * i + j] = op2(c, SPV_OP_F_ADD, c->t_f32, part[i][j], addend);
        }
    }
    u32 d = in->dst;
    if (d >= SM86_RZ) return;
    if (f32)
        for (k = 0; k < 4; k++) wr(c, d + k, to_u(c, acc[k]));
    else
        for (u32 i = 0; i < 2; i++)
            wr(c, d + i, pack_half(c, spv_emit_vec2_construct(&c->e, c->t_v2f, acc[2 * i], acc[2 * i + 1])));
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
   round, in f32 for both D types: with the mixed dot product (g_spirv_translate.mixed_dot) each pair of products goes
   into the accumulator in one instruction, rounded as the device does it (RDNA 3.5's is a few f32 places off an FMA
   chain even on exact sums, within the same bound); without it the pairs are unpacked to f32 and dotted.
   With g_spirv_translate.hmma_pairs, lanes l and l ^ 4 (quads g and g ^ 1, the same t, so the same two B columns) split
   the 16 B words both multiply by: lane l shuffles in those of quad lanes 2 G0 and 2 G0 + 1 (G0 = bit 2 of l) and takes
   its pair's, the other two, with a xor of 4, which RADV makes a DPP move where a shuffle at any index is a permute
   through LDS. Its A pairs follow the same quad lanes, in the same order: quad broadcasts of lanes u and 2 + u, the
   one G0 picks. Only the order of the sums changes, lanes of G0 1 adding quad lanes 2 and 3's pairs first */
/* an HMMA and its two fragments: the words a fused pair's LDSM read are given (a and b, NULL where they come from
   the registers), so that the fused block writes them into the slot without a read of the registers */
static void hmma(Ctx* c, const Sm86Insn* in, const u32* ldsm_a, const u32* ldsm_b) {
    u32 v = sm86_field(in, 72, 19);
    if (v & ~0x18u) {
        fail(c, "HMMA variant %#x", v);
        return;
    }
    int k16 = (int)(v >> 3 & 1), f32 = (int)(v >> 4 & 1);
    u32 ra = sm86_field(in, 24, 8), rb = sm86_field(in, 32, 8), rc = sm86_field(in, 64, 8);
    if (c->coop) {
        hmma_coop(c, in, k16, f32, ra, rb, rc, ldsm_a);
        return;
    }
    if (g_spirv_translate.hmma_astat && !g_spirv_translate.hmma_f16) {
        hmma_astat(c, in, k16, f32, ra, rb, rc);
        return;
    }
    u32 na = k16 ? 4 : 2, nb = k16 ? 2 : 1, ncd = f32 ? 4 : 2, a[4], b[2], cc[4], k;
    for (k = 0; k < na; k++) a[k] = ldsm_a ? ldsm_a[k] : (ra >= SM86_RZ ? c->zero : rd(c, ra + k));
    u32 zero = rb >= SM86_RZ ? 0 : hmma_zero(c);
    for (k = 0; k < nb; k++) {
        b[k] = ldsm_b ? ldsm_b[k] : (rb >= SM86_RZ ? c->zero : rd(c, rb + k));
        if (zero) b[k] = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, b[k], zero);
    }
    for (k = 0; k < ncd; k++) cc[k] = rc >= SM86_RZ ? c->zero : rd(c, rc + k);
    u32 lane = warp_lane(c);
    u32 g4 = 0; /* 4g, which quad_read and a staged A make where they need it */
    u32 t8 = 0; /* (lane % 4) * 8, which the paths that read B by a shuffle or as a pair take: staged B's row is 16t */
    /* the pairs as two f16 for OpFDot2MixAcc32VALVE (a bitcast: no instruction), or unpacked to two f32 */
    int mixed = g_spirv_translate.mixed_dot && g_spirv_translate.float16;
    /* The products and their sum in half floats, two products to an OpFma where the f32 sum takes two instructions
       (g_spirv_translate.hmma_f16), and C added in f32 to that sum at the end: the one speed an emulated tensor core
       finds. Each of the two half sums rounds in f16 once a product (8 times for k16, 4 for k8), so a result is within
       about 2^-8 of its products' magnitudes where the f32 sum is within 2^-20, and 2^-21 more below the normals (more
       where the device flushes f16 subnormals); a product or partial sum past 65504 is an infinity. The driver's matrix
       unit (hmma_coop) is the exact way to that speed and it is off where the driver has no VK_KHR_cooperative_matrix */
    int packed = g_spirv_translate.hmma_f16 && !shader_hmma_f16_unused();
    u32 t_v2h = 0, start2 = 0;
    if (mixed || packed) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_FLOAT16);
        t_v2h = spv_emit_type_vector(&c->e, spv_emit_type_float(&c->e, 16), 2);
    }
    if (packed) { /* -0, the sum's identity, and +0 in one half where C is RZ: the console adds that +0 */
        u32 t_f16 = spv_emit_type_float(&c->e, 16), neg = spv_emit_constant_u32(&c->e, t_f16, 0x8000u);
        start2 = spv_emit_constant_vec2(&c->e, t_v2h, rc < SM86_RZ ? neg : spv_emit_constant_u32(&c->e, t_f16, 0), neg);
    }
    if (mixed) {
        spv_emit_capability(&c->e, SPV_CAPABILITY_DOT_PRODUCT_FLOAT16_ACC_FLOAT32_VALVE);
        spv_emit_extension(&c->e, "SPV_VALVE_mixed_float_dot_product");
    }
    u32 acc[2][2];                        /* [row g, g + 8][column 2t, 2t + 1] */
    u32 pacc[2][2] = {{0, 0}, {0, 0}};    /* packed: the f16 half sums of the same, C's not in them yet */
    if (packed) {
        for (u32 i = 0; i < 2; i++)
            for (u32 j = 0; j < 2; j++) pacc[i][j] = start2;
    } else {
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
    }
    /* staged: lane 4g + p's A words at 16g + 4p, lane 8t + 4j + p's B words at 128 + 16t + 8j + 2p */
    int stage = stage_wanted(c), pairs = !stage && g_spirv_translate.hmma_pairs != 0;
    u32 a_at = 0, b_at = 0;
    if (stage) {
        if (!c->stage_slots) {
            u32 lanes = (u32)(c->si->cs_local[0] * c->si->cs_local[1] * c->si->cs_local[2]);
            u32 array = spv_emit_type_array_u32(&c->e, c->t_u32, (lanes + 31) / 32 * HMMA_STAGE_WORDS);
            c->stage_slots = spv_emit_workgroup_variable(&c->e, spv_emit_type_workgroup_pointer(&c->e, array));
            add_interface(c, c->stage_slots);
        }
        u32 slot = spv_emit_binary(&c->e, SPV_OP_I_MUL, c->t_u32,
                                   spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32,
                                                   builtin_u32(c, &c->local_index, SPV_BUILT_IN_LOCAL_INVOCATION_INDEX),
                                                   cu(c, 5)),
                                   cu(c, HMMA_STAGE_WORDS));
        u32 own_a = spv_emit_iadd_u32(&c->e, c->t_u32, slot,
                                      spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, lane, cu(c, 2)));
        u32 own_b = spv_emit_iadd_u32(
            &c->e, c->t_u32, slot,
            spv_emit_iadd_u32(&c->e, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, lane, cu(c, 1)), cu(c, 128)));
        coop_barrier(c); /* the HMMA before's loads of the slot */
        for (k = 0; k < na; k++) spv_emit_store_nonprivate(&c->e, stage_ptr(c, coop_add(c, own_a, k)), a[k]);
        for (k = 0; k < nb; k++) spv_emit_store_nonprivate(&c->e, stage_ptr(c, coop_add(c, own_b, k)), b[k]);
        coop_barrier(c);
        a_at = spv_emit_iadd_u32(&c->e, c->t_u32, slot,
                                 spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                                 quad_base(c, lane, &g4), cu(c, 2)));
        /* B's row t at 16t: ucas found the two shifts by constants (8t, then one more) one */
        b_at = spv_emit_iadd_u32(
            &c->e, c->t_u32, slot,
            spv_emit_iadd_u32(&c->e, c->t_u32,
                              spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                                              spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3)),
                                              cu(c, 4)),
                              cu(c, 128)));
    }
    u32 g0 = 0, t8h = 0, mine[2][2][2]; /* pairs: [column 2t + j][B's register h][quad lane 2 G0 + u]'s word */
    if (!stage) {
        t8 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32,
                             spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3)), cu(c, 3));
    }
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
            if (stage)
                bits = spv_emit_load_nonprivate(&c->e, c->t_u32, stage_ptr(c, coop_add(c, a_at, 4 * p + k)));
            else if (pairs) {
                u32 x = quad_read(c, a[k], lane, &g4, p & 1), y = quad_read(c, a[k], lane, &g4, 2 + (p & 1));
                bits = spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, g0, p < 2 ? y : x, p < 2 ? x : y);
            } else
                bits = quad_read(c, a[k], lane, &g4, p);
            av[k] = mixed || packed ? spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, bits) : half_pair(c, bits);
        }
        for (u32 j = 0; j < 2; j++)
            for (k = 0; k < nb; k++) {
                u32 bits;
                if (stage)
                    bits =
                        spv_emit_load_nonprivate(&c->e, c->t_u32, stage_ptr(c, coop_add(c, b_at, 8 * j + 2 * p + k)));
                else if (!pairs)
                    bits =
                        warp_read(c, b[k], warp_index(c, t8, 4 * j + p));
                else if (p < 2)
                    bits = mine[j][k][p] =
                        warp_read(c, b[k], warp_index(c, t8h, 4 * j + p));
                else
                    bits = warp_xor(c, mine[j][k][p - 2], 4);
                bv[j][k] = mixed || packed ? spv_emit_unary(&c->e, SPV_OP_BITCAST, t_v2h, bits) : half_pair(c, bits);
            }
        for (u32 h = 0; h < nb; h++) /* the k halves: A's registers 2h (row g) and 2h + 1 (row g + 8), B's h */
            for (u32 i = 0; i < 2; i++)
                for (u32 j = 0; j < 2; j++)
                    if (packed)
                        pacc[i][j] = spv_emit_glsl_ternary(&c->e, t_v2h, c->glsl, SPV_GLSL_FMA, av[2 * h + i],
                                                           bv[j][h], pacc[i][j]);
                    else
                        acc[i][j] = mixed ? SPV(&c->e, SPV_OP_F_DOT2_MIX_ACC32_VALVE, c->t_f32, av[2 * h + i], bv[j][h],
                                                acc[i][j])
                                          : op2(c, SPV_OP_F_ADD, c->t_f32, acc[i][j],
                                                op2(c, SPV_OP_DOT, c->t_f32, av[2 * h + i], bv[j][h]));
    }
    if (packed) { /* the half sums added into one f32, then C: the console's f32 sum up to the half sums' precision */
        for (u32 i = 0; i < 2; i++)
            for (u32 j = 0; j < 2; j++) {
                u32 pair = half_pair(c, spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_u32, pacc[i][j]));
                acc[i][j] = op2(c, SPV_OP_F_ADD, c->t_f32, spv_emit_vec4_extract(&c->e, c->t_f32, pair, 0),
                                spv_emit_vec4_extract(&c->e, c->t_f32, pair, 1));
                if (rc < SM86_RZ)
                    acc[i][j] = op2(c, SPV_OP_F_ADD, c->t_f32, acc[i][j],
                                    f32 ? to_f(c, cc[2 * i + j])
                                        : spv_emit_vec4_extract(&c->e, c->t_f32, half_pair(c, cc[i]), j));
            }
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
            wr(c, d + i, pack_half(c, spv_emit_vec2_construct(&c->e, c->t_v2f, acc[i][0], acc[i][1])));
}
/* a 16-bit element of shared memory at the byte address, in the low half */
static u32 shared_u16(Ctx* c, u32 address) {
    if (g_spirv_translate.hmma_narrow_lds && c->shared16) {
        /* A narrow shared load: read the half at the byte address directly from the u16 view instead of loading its
           whole u32 word and shifting and masking it (Google's mixed-input trick). RADV lowers this u16 OpLoad to
           ds_read_u16, and the load's zero extension puts the half where the fragment wants it with no shift or mask. */
        u32 index = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, address, cu(c, 1));
        index = SPV_EXT(&c->e, c->t_u32, SPV_GLSL_U_MIN, cu(c, c->shared16_words - 1), index);
        u32 ptr = spv_emit_function_element_pointer(&c->e, c->shared16_ptr, c->shared16, index);
        return spv_emit_int_convert(&c->e, c->t_u32,
                                    spv_emit_load_nonprivate(&c->e, c->t_u16, ptr), 0);
    }
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
/* an LDSM's reads: the fragments it fills, one word a matrix. The fields are given, which a fused pair of an LDSM
   and an HMMA (fuse.c) packs into its own (sm86_fuse_ldsm_*) */
static void ldsm_words(Ctx* c, u32 reg, u32 imm, u32 count, int trans, u32* out) {
    shared_var(c); /* before mem_ptr, which bounds the index by its size: as LDS does, for a program whose LDSM comes
                      before any other access to shared memory */
    u32 addr = spv_emit_iadd_u32(&c->e, c->t_u32, rd(c, reg), cu(c, imm));
    u32 lane = warp_lane(c), g = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, lane, cu(c, 2));
    u32 t = spv_emit_binary(&c->e, SPV_OP_BITWISE_AND, c->t_u32, lane, cu(c, 3));
    for (u32 m = 0; m < (1u << count); m++) {
        u32 m8 = 8 * m;
        if (!trans) {
            u32 row = warp_read(c, addr, m8 ? spv_emit_iadd_u32(&c->e, c->t_u32, g, cu(c, m8)) : g);
            u32 at = spv_emit_iadd_u32(&c->e, c->t_u32, row,
                                       spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 2)));
            out[m] = spv_emit_load_nonprivate(&c->e, c->t_u32, mem_ptr(c, 0, at, 0));
        } else {
            u32 t2 = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, t, cu(c, 1));
            u32 r0 = m8 ? spv_emit_iadd_u32(&c->e, c->t_u32, t2, cu(c, m8)) : t2;
            u32 col = spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, g, cu(c, 1));
            u32 lo = shared_u16(c, spv_emit_iadd_u32(&c->e, c->t_u32, warp_read(c, addr, r0), col));
            u32 hi = shared_u16(
                c, spv_emit_iadd_u32(&c->e, c->t_u32,
                                     warp_read(c, addr, spv_emit_iadd_u32(&c->e, c->t_u32, r0, cu(c, 1))), col));
            out[m] = spv_emit_binary(&c->e, SPV_OP_BITWISE_OR, c->t_u32, lo,
                                     spv_emit_binary(&c->e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, hi, cu(c, 16)));
        }
    }
}
/* the LDSM's reads into out (a fused pair's, which its HMMA takes), and the destination registers' words */
static void ldsm_do(Ctx* c, u32 dst, u32 reg, u32 imm, u32 count, int trans, u32* out) {
    u32 out4[4];
    if (!out) out = out4;
    ldsm_words(c, reg, imm, count, trans, out);
    for (u32 m = 0; m < (1u << count); m++)
        wr(c, dst + m, out[m]);
}
static void ldsm(Ctx* c, const Sm86Insn* in) {
    u32 count = sm86_field(in, 72, 2), trans = sm86_field(in, 78, 1);
    if (count > 2 || sm86_field(in, 74, 4) || sm86_field(in, 79, 12) || sm86_field(in, 91, 1)) {
        fail(c, "LDSM variant %#x", sm86_field(in, 72, 20));
        return;
    }
    u32 imm = sm86_field(in, 40, 24), bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    ldsm_do(c, in->dst, sm86_field(in, 24, 8), bias, count, (int)trans, NULL);
}
/* the fused pair of an LDSM and the HMMA that reads its fragments (fuse.c's SM86_LDSM_HMMA): the LDSM's reads of the
   tiles, once, write the destination registers and go to the HMMA as A's words, which it then takes without a read
   of those registers */
static void fused_ldsm_hmma(Ctx* c, const Sm86Insn* in) {
    u32 count = sm86_fuse_ldsm_count(in), trans = sm86_fuse_ldsm_trans(in);
    u32 imm = sm86_field(in, 40, 24), bias = imm & 0x800000u ? imm | 0xff000000u : imm;
    int k16 = (int)(sm86_field(in, 72, 19) >> 3 & 1);
    u32 na = k16 ? 4 : 2, a[4], out[4];
    ldsm_do(c, sm86_fuse_ldsm_dst(in), sm86_fuse_ldsm_a(in), bias, count, trans, out);
    if (c->failed) return;
    if (1u << count < 4u)
        for (u32 k = 1u << count; k < 4; k++) out[k] = c->zero; /* fewer fragments: the rest is zero */
    for (u32 k = 0; k < na; k++) a[k] = out[k];
    hmma(c, in, a, NULL);
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
    if (c->lone) return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, pred, cu(c, 1), c->zero); /* lane 0's bit */
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    u32 all = spv_emit_subgroup_ballot(&c->e, c->t_v4u, c->t_u32, pred);
    u32 half = spv_emit_binary(&c->e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, system_value(c, 0), cu(c, 5));
    return SPV(&c->e, SPV_OP_VECTOR_EXTRACT_DYNAMIC, c->t_u32, all, half);
}

/* component k of generic input i as Vulkan interpolates it, at the centroid (loc 1) or the default place */
static u32 ipa_attr(Ctx* c, u32 i, u32 k, u32 loc) {
    u32 v = to_f(c, input_load(c, i, k));
    if (loc == 1 && c->in_attr[i] && !(c->centroid_mask & (1u << i))) {
        /* a variable has one location: centroid when any IPA of the input asks for it. An input that no stage
           before writes (in_unfed_mask) is a constant with no variable: a decoration of it named id 0, a module
           that spirv-val and the validation layer refuse */
        spv_emit_decorate(&c->e, c->in_attr[i], SPV_DECORATION_CENTROID, NULL, 0);
        c->centroid_mask |= 1u << i;
    }
    return v;
}
/* IPA of component k of generic input i, with the program's multiply by w taken back for a perspective input of
   frequency 0 */
static u32 ipa_input(Ctx* c, u32 i, u32 k, u32 loc, u32 freq) {
    u32 v = ipa_attr(c, i, k, loc);
    /* The hardware interpolates attr/w for a perspective input, and the program multiplies by w (the reciprocal of
       a[0x7c]). Vulkan's input is attr itself: times the interpolated 1/w, FragCoord.w */
    if (interp_mode(c, i) == 2 && freq == 0) {
        v = spv_emit_binary(&c->e, SPV_OP_F_MUL, c->t_f32, v, frag_coord(c, 3));
        spv_emit_no_contraction(&c->e, v);
    }
    return v;
}
/* the last instruction before i in program order that writes register r (writes_word), -1 for none */
static int nearest_write(Ctx* c, u32 i, u32 r) {
    for (u32 j = i; j-- > 0;)
        if (writes_word(&c->in[j], r)) return (int)j;
    return -1;
}
/* the attribute address the IPA at k reads where ipa_input multiplies it by FragCoord.w (a perspective generic at
   frequency 0, read as the IPA case reads it, which refuses the program for the IPA forms it doesn't take), 0 for
   another */
static u32 ipa_over_w(Ctx* c, u32 k) {
    const Sm86Insn* in = &c->in[k];
    if (in->op != SM86_IPA || sm86_field(in, 78, 2)) return 0;
    u32 addr = sm86_field(in, 64, 8) * 4;
    if (shader_legacy_group(addr) >= 0) addr = shader_legacy_address(addr);
    return addr >= 0x80 && addr < 0x280 && interp_mode(c, (addr - 0x80) >> 4) == 2 ? addr : 0;
}
/* the FMUL at i, with no scale, of two registers with no modifier: one an IPA's that ipa_input multiplied by
   FragCoord.w (ipa_over_w), the other MUFU.RCP, with no modifier, of IPA's read of a[0x7c] at frequency 0, which is
   FragCoord.w. Each the one write that reaches its use however control went (reaching_write). That product is the
   input itself: as Vulkan interpolates it, with neither multiply, whose roundings (the FMUL's in its own mode, toward
   zero in one game's programs) and infinity past the float range it doesn't take. 0 for another FMUL */
static u32 fmul_over_w(Ctx* c, u32 i) {
    const Sm86Insn* in = &c->in[i];
    if (sm86_field(in, 84, 3) != 4 || in->b.kind != SM86_SRC_REG || in->a.neg || in->a.abs || in->b.neg || in->b.abs)
        return 0;
    if (!c->over_w_known) {
        for (u32 j = 0; j < c->n; j++) {
            const Sm86Insn* x = &c->in[j];
            if (x->dst >= SM86_RZ) continue;
            if (ipa_over_w(c, j)) c->over_w_ipa[x->dst >> 5] |= 1u << (x->dst & 31);
            if (x->op == SM86_MUFU && sm86_field(x, 74, 6) == 4) c->over_w_rcp[x->dst >> 5] |= 1u << (x->dst & 31);
        }
        c->over_w_known = 1;
    }
    for (u32 side = 0; side < 2; side++) {
        /* each operand's write before it in program order first, which costs no walk of the flow, then the walks that
           prove it the one that reaches it */
        u32 ra = side ? in->b.reg : in->a.reg, rb = side ? in->a.reg : in->b.reg;
        if (!(c->over_w_ipa[ra >> 5] >> (ra & 31) & 1) || !(c->over_w_rcp[rb >> 5] >> (rb & 31) & 1)) continue;
        int k = nearest_write(c, i, ra), m = nearest_write(c, i, rb);
        u32 addr = k >= 0 ? ipa_over_w(c, (u32)k) : 0;
        if (!addr || m < 0) continue;
        const Sm86Insn* r = &c->in[m];
        if (r->op != SM86_MUFU || sm86_field(r, 74, 6) != 4 || sm86_field(r, 73, 1) || r->b.kind != SM86_SRC_REG ||
            r->b.neg || r->b.abs)
            continue;
        int w = nearest_write(c, (u32)m, r->b.reg);
        if (w < 0) continue;
        const Sm86Insn* v = &c->in[w];
        if (v->op == SM86_IPA && sm86_field(v, 64, 8) * 4 == 0x7c && !sm86_field(v, 78, 2) &&
            reaching_write(c, i, ra) == k && reaching_write(c, i, rb) == m && reaching_write(c, (u32)m, r->b.reg) == w)
            return ipa_attr(c, (addr - 0x80) >> 4, (addr >> 2) & 3, sm86_field(&c->in[k], 76, 2));
    }
    return 0;
}

/* ---- one instruction ---- */

/* the fragment program's outputs from the registers when it ends: the colours of the render targets the SPH's omap
   enables, four registers a target, then the sample mask's register and the depth's after it (Maxwell's layout, as
   emit_flow.c reads it: the depth is the last colour register plus two whether the program writes a sample mask or
   not, as nouveau's nvc0_fp_assign_output_slots has it from Kepler on). An integer target (the key's rt_int_mask,
   signed in rt_sint_mask) takes the registers' bits in an output of its own numeric type: Vulkan leaves a float
   output's values undefined in an integer attachment */
static void epilogue(Ctx* c) {
    if (c->stage == ST_VS && c->position && !(c->si->clip_enable & SNX_SH_NOT_LAST)) vertex_depth_fix(c);
    if (c->stage == ST_GS && c->passthrough) gs_passthrough(c);
    if (c->stage != ST_FS) return;
    u32 target = c->sph[18], reg = 0;
    for (u32 rt = 0; rt < 8; rt++) {
        if (!((target >> (rt * 4)) & 15)) continue;
        int integer = c->si->rt_int_mask >> rt & 1, sign = integer && (c->si->rt_sint_mask >> rt & 1);
        u32 type = !integer ? c->t_v4f : sign ? spv_emit_type_vec4(&c->e, c->t_i32) : c->t_v4u;
        if (!c->color[rt]) {
            c->color[rt] = spv_emit_output_vec4(&c->e, type, rt);
            add_interface(c, c->color[rt]);
        }
        u32 v[4];
        for (u32 k = 0; k < 4; k++) {
            u32 bits = rd(c, reg + k);
            v[k] = !integer ? to_f(c, bits) : sign ? spv_emit_unary(&c->e, SPV_OP_BITCAST, c->t_i32, bits) : bits;
        }
        spv_emit_store(&c->e, c->color[rt], spv_emit_vec4_construct(&c->e, type, v));
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
/* a fragment's size in pixels as NAK_SV_VARIABLE_RATE packs it: the width in bits 8-15, the height in bits 16-23 */
static u32 frag_size(Ctx* c) {
    SpvEmitter* e = &c->e;
    int how = g_spirv_translate.shading_rate;
    if (how == SH_RATE_NONE) return cu(c, 1u << 8 | 1u << 16);
    if (!c->frag_size) {
        if (how == SH_RATE_NV) {
            spv_emit_extension(e, "SPV_NV_shading_rate");
            spv_emit_capability(e, SPV_CAPABILITY_SHADING_RATE_NV);
            c->frag_size = spv_emit_interface_variable(e, SPV_STORAGE_INPUT, c->t_v2u, c->t_u32, 0,
                                                       SPV_BUILT_IN_FRAGMENT_SIZE_NV, 0, 0);
        } else {
            spv_emit_extension(e, "SPV_KHR_fragment_shading_rate");
            spv_emit_capability(e, SPV_CAPABILITY_FRAGMENT_SHADING_RATE_KHR);
            c->frag_size = spv_emit_builtin_input_u32(e, c->t_u32, SPV_BUILT_IN_SHADING_RATE_KHR);
        }
        spv_emit_flat(e, c->frag_size);
        add_interface(c, c->frag_size);
    }
    u32 w, h;
    if (how == SH_RATE_NV) {
        u32 v = spv_emit_load(e, c->t_v2u, c->frag_size);
        w = spv_emit_vec4_extract(e, c->t_u32, v, 0);
        h = spv_emit_vec4_extract(e, c->t_u32, v, 1);
    } else {
        /* log2 of the height in bits 0-1 (Vertical2Pixels, Vertical4Pixels), of the width in bits 2-3 */
        u32 v = spv_emit_load(e, c->t_u32, c->frag_size);
        w = spv_emit_binary(e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, cu(c, 1),
                            spv_emit_binary(e, SPV_OP_BITWISE_AND,
                                            c->t_u32, spv_emit_binary(e, SPV_OP_SHIFT_RIGHT_LOGICAL, c->t_u32, v, cu(c, 2)),
                                            cu(c, 3)));
        h = spv_emit_binary(e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, cu(c, 1),
                            spv_emit_binary(e, SPV_OP_BITWISE_AND, c->t_u32, v, cu(c, 3)));
    }
    return spv_emit_binary(e, SPV_OP_BITWISE_OR, c->t_u32,
                           spv_emit_binary(e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, w, cu(c, 8)),
                           spv_emit_binary(e, SPV_OP_SHIFT_LEFT_LOGICAL, c->t_u32, h, cu(c, 16)));
}
static u32 system_value(Ctx* c, u32 sr) {
    switch (sr) {
        case 0x1d:
            /* SR_INVOCATION_INFO of a geometry program: the input primitive's vertices in bits 16-23 and the
               primitive's index in the batch (bits 0-7) 0, which makes the index the program builds for ISBERD the
               vertex's (sail/sm53_thread.sail has Maxwell's the same) */
            if (c->stage != ST_GS) {
                fail(c, "S2R system register 0x1d outside a geometry program");
                return c->zero;
            }
            return cu(c, gs_vertices(c) << 16);
        case 0x00: /* LANEID */
            return c->lone ? c->zero : native_lane(c);
        case 0x21:
        case 0x22:
        case 0x23: return builtin_vec3(c, &c->local_id, SPV_BUILT_IN_LOCAL_INVOCATION_ID, sr - 0x21);
        case 0x25:
        case 0x26:
        case 0x27: return builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, sr - 0x25);
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
               in bits 16-23 (1, 2 or 4), which nak_nir.c reads for load_frag_shading_rate. One game's fragment
               programs take its width with PRMT R, R, 0x7771, RZ. The device's own size of the fragment, as an
               Ounce's shading rate image gives it (vk/shading_rate.c), else 1 by 1 */
            if (c->stage != ST_FS) {
                fail(c, "S2R system register 0x84 outside a fragment program");
                return c->zero;
            }
            return frag_size(c);
        case 0x1e:
        case 0x1f:
            /* SR_WSCALEFACTOR_XY and SR_WSCALEFACTOR_Z (NAK's numbering): the factors of clip space w scaling, by
               which one game's vertex programs scale w before they test the position against it (FMUL by the
               register, then FSETP, then PSETP). The runtime sets no w scaling, which is a factor of 1 */
            return cu(c, 0x3f800000u);
        case 0x13:
            /* SR_THREAD_KILL: all ones in a helper lane, which is also a lane that KILL demoted (as the Maxwell
               translator gives it). Five fragment programs of one game read it, and their draws were skipped */
            if (c->stage != ST_FS) {
                fail(c, "S2R system register 0x13 outside a fragment program");
                return c->zero;
            }
            spv_emit_capability(&c->e, SPV_CAPABILITY_DEMOTE_TO_HELPER_INVOCATION); /* IsHelperInvocationEXT */
            return spv_emit_ternary(&c->e, SPV_OP_SELECT, c->t_u32, spv_emit_is_helper(&c->e, c->t_bool),
                                    cu(c, 0xffffffffu), c->zero);
        default: fail(c, "S2R system register 0x%x", sr); return c->zero;
    }
}
/* ---- a stage with no warp of 32 lanes ----

   A program reads its warp through VOTE, SHFL, MATCH, REDUX, the tensor core's instructions, LANEID and the lane masks.
   Where the device's subgroups have not 32 lanes (lanes, sm86.h) a vertex, geometry or fragment program that reads one
   runs each invocation as a warp whose only lane is lane 0, as the console runs a batch of one vertex or primitive: its
   ballot is its own bit, SHFL keeps its own value, MATCH finds itself and a reduction is its value, which is the
   console's for a lane alone or one out of range. A fragment's reads inside its quad keep the quad's lanes where the
   subgroups hold whole quads. A compute program's warps are whole, so it is refused (lane_gate) */
enum { LANE_NONE, LANE_PLACE, LANE_QUAD, LANE_WARP, LANE_MATRIX };
/* a SHFL whose lanes are its own quad's: segments of 4 (c, an immediate, 0x1c03) */
static int shfl_in_quad(const Sm86Insn* in) {
    return (in->op == SM86_SHFL_II || in->op == SM86_SHFL_RI) && sm86_field(in, 40, 13) == 0x1c03;
}
/* what an instruction takes of the invocation's warp */
static int lane_use(const Ctx* c, const Sm86Insn* in) {
    switch (in->op) {
        case SM86_VOTE:
        case SM86_UVOTE:
        case SM86_MATCH:
        case SM86_REDUX: return LANE_WARP;
        case SM86_SHFL_II:
        case SM86_SHFL_RI:
        case SM86_SHFL_IR:
        case SM86_SHFL_RR: return c->stage == ST_FS && shfl_in_quad(in) ? LANE_QUAD : LANE_WARP;
        case SM86_FSWZADD: return LANE_QUAD;
        case SM86_HMMA:
        case SM86_LDSM:
        case SM86_MOVM: return LANE_MATRIX;
        case SM86_S2R:
        case SM86_S2UR: {
            u32 sr = sm86_field(in, 72, 8);
            return sr == 0 || (sr >= 0x38 && sr <= 0x3c) ? LANE_PLACE : LANE_NONE;
        }
        default: return LANE_NONE;
    }
}
/* whether a SHFL reads real lanes: the warp's, or in a lone fragment its quad's */
static int shfl_native(const Ctx* c, const Sm86Insn* in) { return !c->lone || (c->stage == ST_FS && shfl_in_quad(in)); }
/* the lane of SHFL and FSWZADD: the device's where the program reads real lanes */
static u32 native_lane(Ctx* c) {
    spv_emit_capability(&c->e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
    return builtin_u32(c, &c->lane_id, SPV_BUILT_IN_SUBGROUP_LOCAL_INVOCATION_ID);
}
/* sets lone, or refuses the first instruction that reads what the stage has not */
static void lane_gate(Ctx* c) {
    if (c->stage == ST_TS || c->stage == ST_MS || (c->lanes & SM86_LANES_WARP)) return;
    for (u32 i = 0; i < c->n; i++) {
        const Sm86Insn* in = &c->in[i];
        int use = lane_use(c, in);
        if (use == LANE_NONE || (use == LANE_QUAD && c->stage == ST_FS && (c->lanes & SM86_LANES_QUAD))) continue;
        if (use == LANE_MATRIX || use == LANE_QUAD) {
            fail(c, "subgroup ballot/shuffle unavailable: %s at %x", sm86_ops[in->op].name, i * 16);
            return;
        }
        c->lone = 1;
    }
    if (c->lone && c->stage == ST_CS) c->notes = &c->lone_notes;
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
    coop_branch(c, l->header);
    coop_label(c, l->header);
    u32 i = spv_emit_load(e, c->t_u32, l->counter);
    u32 more = spv_emit_binary(e, SPV_OP_U_LESS_THAN, c->t_bool, i, end);
    spv_emit_loop_merge(e, l->merge, l->cont);
    coop_branch_if(c, more, body, l->merge);
    coop_label(c, body);
    return i;
}
static void loop_end(Ctx* c, const MeshLoop* l, u32 i) {
    SpvEmitter* e = &c->e;
    coop_branch(c, l->cont);
    coop_label(c, l->cont);
    spv_emit_store(e, l->counter, spv_emit_iadd_u32(e, c->t_u32, i, l->step));
    coop_branch(c, l->header);
    coop_label(c, l->merge);
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
                    spv_emit_decorate(e, generic[i], SPV_DECORATION_PER_PRIMITIVE_EXT, NULL, 0);
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
    coop_label(c, spv_emit_id(e));
    MeshLoop loop = {0};
    if (c->stage == ST_MS) loop.counter = spv_emit_function_variable(e, spv_emit_type_function_pointer(e, c->t_u32));
    /* the groups the dispatch and its mesh draws share, in a module variable (payload_groups) */
    if (c->payload_buf) (void)payload_groups(c);
    loop.step = cu(c, m->threads);
    u32 invocation = builtin_u32(c, &c->local_index, SPV_BUILT_IN_LOCAL_INVOCATION_INDEX);
    u32 first = spv_emit_binary(e, SPV_OP_I_EQUAL, c->t_bool, invocation, c->zero);
    if (c->payload_buf) {
        u32 groups = payload_groups_compute(c);
        spv_emit_store(e, c->payload_groups, groups);
        /* The compute-task path draws every mesh workgroup of the grid of the most any task workgroup launched: one at
           or past its task workgroup's count was not launched, and produces nothing */
        if (c->stage == ST_MS && m->has_task) {
            u32 index = builtin_vec3(c, &c->group_id, SPV_BUILT_IN_WORKGROUP_ID, 0);
            u32 count = spv_emit_load_nonprivate(e, c->t_u32, payload_count_ptr(c, payload_slot_index(c, groups)));
            u32 launched = spv_emit_binary(e, SPV_OP_U_LESS_THAN, c->t_bool, index, count);
            u32 run = spv_emit_id(e), out = spv_emit_id(e);
            coop_merge(c, out);
            coop_branch_if(c, launched, run, out);
            coop_label(c, out);
            /* the workgroup produces nothing, and says so: RADV hangs a mesh workgroup that returns without
               OpSetMeshOutputsEXT */
            SPV_VOID(e, SPV_OP_SET_MESH_OUTPUTS_EXT, c->zero, c->zero);
            coop_end(c);
            coop_label(c, run);
        }
    }
    u32 then = spv_emit_id(e), merge = spv_emit_id(e);
    coop_merge(c, merge);
    coop_branch_if(c, first, then, merge);
    coop_label(c, then);
    if (c->stage == ST_TS && c->payload_buf)
        payload_store(c, cu(c, 1), c->zero);
    else if (c->stage == ST_TS)
        spv_emit_store(e, isbe_ptr(c, payload_var(c), SPV_STORAGE_TASK_PAYLOAD, cu(c, 1)), c->zero);
    else
        spv_emit_store(e, isbe_ptr(c, map_var(c), SPV_STORAGE_WORKGROUP, cu(c, 3)), c->zero);
    coop_branch(c, merge);
    coop_label(c, merge);
    spv_emit_control_barrier(e, c->t_u32);
    SPV(e, SPV_OP_FUNCTION_CALL, c->t_void, body);
    spv_emit_control_barrier(e, c->t_u32);
    if (c->stage == ST_TS) {
        u32 count = payload_load(c, cu(c, 1));
        /* within the device's limit: a count past it is undefined behavior, which can hang the GPU */
        if (g_spirv_translate.mesh_tasks_max)
            count = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, count,
                                         cu(c, g_spirv_translate.mesh_tasks_max));
        if (!c->payload_buf) {
            SPV_VOID(e, SPV_OP_EMIT_MESH_TASKS_EXT, count, cu(c, 1), cu(c, 1), payload_var(c));
        } else {
            /* The compute-task path: the groups this workgroup launched, in its slot, and the draw's record, which
               the mesh workgroups read instead of the payload the hardware would copy: the most any one workgroup
               launched (x), the task workgroups (y) and 1 (z) */
            u32 groups = payload_groups(c);
            u32 group = payload_slot_index(c, groups);
            u32 wrote = spv_emit_id(e), ready = spv_emit_id(e);
            coop_merge(c, ready);
            coop_branch_if(c, first, wrote, ready);
            coop_label(c, wrote);
            spv_emit_store_nonprivate(e, payload_count_ptr(c, group), count);
            spv_emit_atomic_u32(e, c->t_u32, SPV_OP_ATOMIC_U_MAX, payload_record_ptr(c, 0), count);
            u32 none = spv_emit_binary(e, SPV_OP_I_EQUAL, c->t_bool, group, c->zero);
            u32 y_then = spv_emit_id(e), y_merge = spv_emit_id(e);
            coop_merge(c, y_merge);
            coop_branch_if(c, none, y_then, y_merge);
            coop_label(c, y_then);
            u32 num = builtin_vec3(c, &c->num_groups, SPV_BUILT_IN_NUM_WORKGROUPS, 0);
            num = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, num, groups);
            spv_emit_store_nonprivate(e, payload_record_ptr(c, 1), num);
            spv_emit_store_nonprivate(e, payload_record_ptr(c, 2), cu(c, 1));
            spv_emit_store_nonprivate(e, payload_record_ptr(c, 3), c->zero);
            coop_branch(c, y_merge);
            coop_label(c, y_merge);
            coop_branch(c, ready);
            coop_label(c, ready);
            spv_emit_return(e); /* the merge block of the count's selection ends the invocation */
        }
        spv_emit_function_end(e);
        return fn;
    }
    u32 map = map_var(c);
    u32 written = spv_emit_load(e, c->t_u32, isbe_ptr(c, map, SPV_STORAGE_WORKGROUP, cu(c, 3)));
    u32 primitives = spv_emit_glsl_binary(e, c->t_u32, c->glsl, SPV_GLSL_U_MIN, written, cu(c, m->max_primitives));
    SPV_VOID(e, SPV_OP_SET_MESH_OUTPUTS_EXT, cu(c, m->max_vertices), primitives);
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
                                              SPV_BUILT_IN_PRIMITIVE_TRIANGLE_INDICES_EXT, 0, 0);
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
    coop_end(c);
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

/* The instructions sm86.sail says (src/nx/gpu/shader/sail/), which sailgen's GPU mode writes as emitters over the
   accessors above (sail/sm86_spirv.h): sg86_emit runs the one of an opcode and says whether there is one */
#include "sm86_sail.inc"

/* the fusion of sm86.roots that instruction cur starts, if one does: the instructions that may take part are cur and
   those after it that run as it does, unguarded, and that no branch lands on (in the same block). Those it takes are
   marked in fused, whose writes it made: 1 when it ran */
static int emit_fused(Ctx* c, SgG* g) {
    int ops[4];
    u32 avail = 0, i = c->cur;
    if (c->cap_on || i >= c->n || &c->in[i] != g->in || !sg86_fuse_starts(g->in->op) || !flow_sources(c)) return 0;
    while (avail < 4 && i + avail < c->n) {
        const Sm86Insn* in = &c->in[i + avail];
        if (in->pred != SM86_PT || in->pred_not) break;
        if (avail && c->jump_first[i + avail] != c->jump_first[i + avail + 1]) break;
        g->at[avail] = in;
        ops[avail++] = in->op;
    }
    if (avail < 2) return 0;
    u32 len = sg86_fuse(g, ops, avail);
    if (!len) return 0;
    if (!c->fused) c->fused = calloc(c->n, 1);
    if (!c->fused) {
        fail(c, "out of memory");
        return 1;
    }
    for (u32 k = 1; k < len; k++) c->fused[i + k] = 1;
    return 1;
}

static void emit_insn(Ctx* c, const Sm86Insn* in) {
    SgG g;
    if (c->fused && c->cur < c->n && &c->in[c->cur] == in && c->fused[c->cur]) return; /* a fusion before wrote it */
    sg86_begin(&g, c, in);
    if (!c->notes && emit_fused(c, &g)) return;
    if (!sg86_emit(&g, in->op)) fail(c, "%s", sm86_ops[in->op].name); /* an instruction sm86.sail does not say */
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
    coop_branch(c, label);
    se->open = 0;
}
static void se_break(Ctx* c, StructEmit* se, u32 scope) {
    se->used[scope] = 1;
    se_branch(c, se, se->label[4 * scope + 3]);
}
static void se_ensure_open(Ctx* c, StructEmit* se) {
    if (se->open) return;
    coop_label(c, spv_emit_id(&c->e));
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
    coop_merge(c, go);
    coop_branch_if(c, set, pending, go);
    coop_label(c, pending);
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
        coop_merge(c, merge);
        coop_branch_if(c, arrived, again, out);
        coop_label(c, again);
        spv_emit_store(&c->e, se->flag, c->zero);
        coop_branch(c, se->label[4 * parent + 2]);
        coop_label(c, out);
        se_break(c, se, parent);
        coop_label(c, merge);
        spv_emit_void_op(&c->e, SPV_OP_UNREACHABLE, NULL, 0);
    } else if (s->arrive) {
        spv_emit_store(&c->e, se->flag, c->zero);
        coop_branch(c, se->label[4 * parent + 2]);
    } else
        se_break(c, se, parent);
    coop_label(c, go);
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
                if (se->open) coop_branch(c, l[0]);
                coop_label(c, l[0]);
                spv_emit_loop_merge(&c->e, l[3], l[2]);
                coop_branch(c, l[1]);
                coop_label(c, l[1]);
                se->open = 1;
                break;
            case SHS_CLOSE:
                if (se->open) se_break(c, se, t->a);
                coop_label(c, l[2]);
                coop_branch(c, l[0]);
                coop_label(c, l[3]);
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
                coop_merge(c, sel[0]);
                coop_branch_if(c, c->branch_cond ? c->branch_cond : c->true_v, then, sel[1]);
                coop_label(c, then);
                se->open = 1;
                break;
            }
            case SHS_ELSE:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                coop_label(c, sel[1]);
                se->open = 1;
                break;
            case SHS_ENDIF:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                coop_label(c, sel[0]);
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
                coop_merge(c, sel[0]);
                coop_switch(c, c->branch_target, sel[1], literals, labels, count);
                se->open = 0;
                break;
            }
            case SHS_CASE:
            case SHS_DEFAULT:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                coop_label(c, t->kind == SHS_DEFAULT ? sel[1] : sel[1] + 1 + t->a);
                se->open = 1;
                break;
            case SHS_ENDSWITCH:
                if (se->open) {
                    *sel_used = 1;
                    se_branch(c, se, sel[0]);
                }
                coop_label(c, sel[0]);
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
/* whether the instruction is ptxas's times-4 of a switch index, into its own register: an IMAD by 4 or a 32-bit
   SHF.L by 2, of a register, with no guard */
static int jump_table_times_4(const Sm86Insn* mul) {
    int times_4 = (mul->op == SM86_IMAD && !sm86_field(mul, 74, 1) && mul->b.value == 4) ||
                  (mul->op == SM86_SHF && !sm86_field(mul, 76, 1) && !sm86_field(mul, 80, 1) &&
                   sm86_field(mul, 73, 2) >= 2 && mul->b.value == 2);
    return times_4 && mul->pred == SM86_PT && !mul->pred_not && mul->a.kind == SM86_SRC_REG &&
           mul->b.kind == SM86_SRC_IMM && mul->c.kind == SM86_SRC_REG && mul->c.reg == SM86_RZ;
}
/* the entries that ptxas's bound of a switch's index names, 0 when the instruction isn't that: an unsigned IMNMX with
   an immediate */
static u32 jump_table_bound_entries(const Sm86Insn* bound) {
    if (bound->op != SM86_IMNMX || bound->pred != SM86_PT || bound->pred_not || sm86_field(bound, 73, 1) ||
        sm86_field(bound, 87, 3) != SM86_PT || sm86_field(bound, 90, 1) || bound->b.kind != SM86_SRC_IMM ||
        bound->b.value >= 64)
        return 0;
    return bound->b.value + 1;
}
/* The entries of the jump table that the LDC at k indexes with register r, as ptxas bounds a switch's index: an
   unsigned IMNMX with an immediate, then an IMAD by 4 or a 32-bit SHF.L by 2 into r (index = min(x, last) * 4). 0 when
   it isn't that in the sixteen instructions before the load */
static u32 jump_table_entries(Ctx* c, u32 k, u32 r) {
    int m = writer_before(c, k, r);
    if (m < 0 || !jump_table_times_4(&c->in[m])) return 0;
    int b = writer_before(c, (u32)m, c->in[m].a.reg);
    return b < 0 ? 0 : jump_table_bound_entries(&c->in[b]);
}
/* whether the instruction writes uniform register r: the uniform ALU's, and the loads and moves into one */
static int writes_uniform(const Sm86Insn* w, u32 r) {
    if (w->op == SM86_UISETP) return 0; /* it writes predicates */
    if (sm86_ops[w->op].kind == SM86_K_UALU) return w->dst == r;
    return (w->op == SM86_ULDC || w->op == SM86_ULDC_IDX || w->op == SM86_ULDCG || w->op == SM86_S2UR ||
            w->op == SM86_R2UR || w->op == SM86_UVOTE) &&
           sm86_field(w, 16, 6) == r;
}
/* writes_uniform, with the high word of a 64-bit load as a write of its own: a BRX's index register may be that word */
static int writes_uniform_word(const Sm86Insn* w, u32 r) {
    if (w->op == SM86_ULDC || w->op == SM86_ULDC_IDX) {
        u32 d = sm86_field(w, 16, 6);
        return r == d || (r == d + 1 && sm86_field(w, 73, 3) == 5 && d + 1 < SM86_URZ);
    }
    return writes_uniform(w, r);
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
/* The LDC or ULDC.IDX of the table of the BRX at i, from the scan of its block: its index, or -1 */
static int brx_scan_load(Ctx* c, u32 i) {
    const Sm86Insn* in = &c->in[i];
    int uniform = in->op == SM86_BRXU;
    u32 reg = sm86_field(in, 24, uniform ? 6 : 8);
    for (u32 k = i; k-- > 0 && k + 32 > i;) {
        const Sm86Insn* w = &c->in[k];
        if (w->op == SM86_BRA || w->op == SM86_EXIT || is_brx(w->op)) break;
        if (uniform) { /* BRXU: the ULDC.IDX that writes its uniform register */
            if (!writes_uniform(w, reg)) continue;
            if (w->op != SM86_ULDC_IDX || sm86_field(w, 73, 3) != 4) break;
        } else {
            if (w->dst != reg || sm86_ops[w->op].kind == SM86_K_UALU) continue;
            if (w->op != SM86_LDC || sm86_field(w, 73, 3) != 4 || sm86_field(w, 78, 2)) break;
        }
        return (int)k;
    }
    return -1;
}
static int brx_load(Ctx* c, u32 i, const u32* first, const u32* from);
/* The entries of the table of the load at k that the next table in its bank starts, as ptxas puts adjacent switches'
   tables one after another: an unclamped read would take that table's words as its own (shader_brx_targets clamps
   Maxwell's so). A later table is the one whose load reaches its BRX over the flow, or, without the flow, the scan of
   its block. max when no other table is found */
static u32 brx_table_limit(Ctx* c, u32 k, u32 max, const u32* first, const u32* from) {
    const Sm86Insn* w = &c->in[k];
    u32 bank = sm86_field(w, 54, 5), off = sm86_field(w, 38, 16), limit = max;
    for (u32 i = 0; i < c->n; i++) {
        if (!is_brx(c->in[i].op) || sm86_never(&c->in[i])) continue;
        int other = first ? brx_load(c, i, first, from) : brx_scan_load(c, i);
        if (other < 0 || (u32)other == k) continue;
        const Sm86Insn* o = &c->in[other];
        u32 ooff = sm86_field(o, 38, 16), words;
        if (sm86_field(o, 54, 5) != bank || ooff < off + 4) continue;
        words = (ooff - off) / 4;
        if (words < limit) limit = words;
    }
    return limit;
}
/* The targets that the jump table of the load at k names, read through g_shader_cb_read as spirv_translate.c reads
   Maxwell's: as many entries as ptxas's bound on the index says, else up to the first word that is no target, or the
   next table in the bank. Two tables one after the other were read as one, and gave a BRX the other's targets as its
   own. The count, or 0 */
static u32 brx_read_words(Ctx* c, u32 k, u32 bias, u32 entries, u32* targets, u32 max, const u32* first,
                          const u32* from) {
    const Sm86Insn* w = &c->in[k];
    u32 bank = sm86_field(w, 54, 5), off = sm86_field(w, 38, 16), count = 0;
    if (entries > max) {
        fail(c, "BRX with a jump table of %u entries", entries);
        return 0;
    }
    u32 limit = entries ? entries : brx_table_limit(c, k, max, first, from);
    for (; count < limit; count++) {
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
        fail(c, "BRX with no jump table in c[%u][0x%x] (%s 0x%x)", bank, off, read ? "word" : "no word, unread", first);
    }
    return count;
}
/* brx_read_words, with the entries that the scan of the load's block gives */
static u32 brx_read_table(Ctx* c, u32 k, u32 bias, u32* targets, u32 max) {
    const Sm86Insn* w = &c->in[k];
    u32 entries = w->op == SM86_ULDC_IDX ? uniform_table_entries(c, k, sm86_field(w, 24, 6))
                                         : jump_table_entries(c, k, sm86_field(w, 24, 8));
    return brx_read_words(c, k, bias, entries, targets, max, NULL, NULL);
}
/* The predecessors of each instruction from the BRAs and the jump tables that brx_resolve has found: first[k] to
   first[k + 1] index from[]. first is malloc'd (n + 1 + the edges words, from after them), NULL when memory runs out */
static u32* brx_preds(Ctx* c, u32** from_out) {
    u32 n = c->n, total = 0;
    for (u32 i = 0; i < n; i++) {
        const Sm86Insn* in = &c->in[i];
        if (sm86_never(in)) continue;
        if (in->op == SM86_BRA)
            total++;
        else if (is_brx(in->op))
            total += c->brx_table[i].count;
    }
    u32* first = calloc((size_t)n + 1 + total + 1, sizeof *first);
    if (!first) return NULL;
    u32 *from = first + n + 1, *fill = calloc((size_t)n + 1, sizeof *fill);
    if (!fill) {
        free(first);
        return NULL;
    }
    for (int pass = 0; pass < 2; pass++)
        for (u32 i = 0; i < n; i++) {
            const Sm86Insn* in = &c->in[i];
            if (sm86_never(in)) continue;
            if (in->op == SM86_BRA) {
                u32 t = sm86_branch_target(in, i);
                if (t >= n) continue;
                if (pass == 0)
                    first[t + 1]++;
                else
                    from[first[t] + fill[t]++] = i;
            } else if (is_brx(in->op)) {
                const BrxTable* table = &c->brx_table[i];
                for (u32 k = 0; k < table->count; k++) {
                    u32 t = table->targets[k];
                    if (t >= n) continue;
                    if (pass == 0)
                        first[t + 1]++;
                    else
                        from[first[t] + fill[t]++] = i;
                }
            }
            if (pass == 0 && i + 1 == n)
                for (u32 k = 0; k < n; k++) first[k + 1] += first[k];
        }
    free(fill);
    *from_out = from;
    return first;
}
/* the instructions before k on each path, walked back over the flow that brx_preds holds: k - 1 when it goes on to
   k, and the jumps to k */
#define BRX_PREDS(k)                                                                                                   \
    do {                                                                                                               \
        if ((k) == 0) ok = 0; /* the program's start: r is not written on this path */                                 \
        if ((k) > 0 && falls_through(&c->in[(k) - 1]) && !seen[(k) - 1]) seen[(k) - 1] = 1, stack[top++] = (k) - 1;    \
        for (u32 j = first[k]; j < first[(k) + 1]; j++)                                                                \
            if (!seen[from[j]]) seen[from[j]] = 1, stack[top++] = from[j];                                             \
    } while (0)
/* The one instruction that writes register r of the regular file, or of the uniform one, that reaches instruction i on
   every path there, without a guard: its index, or -1 when that can't be shown */
static int brx_writer(Ctx* c, const u32* first, const u32* from, u32 i, u32 r, int uniform) {
    u32 n = c->n, *stack = malloc(((size_t)n + 1) * sizeof *stack), top = 0;
    u8* seen = calloc((size_t)n + 1, 1);
    if (!stack || !seen) {
        free(stack);
        free(seen);
        return -2; /* out of memory, not a value the flow can't name */
    }
    int ok = 1, found = -1;
    BRX_PREDS(i);
    while (ok && top) {
        u32 k = stack[--top];
        const Sm86Insn* in = &c->in[k];
        if (!(uniform ? writes_uniform_word(in, r) : writes_word(in, r))) {
            BRX_PREDS(k);
            continue;
        }
        if (in->pred != SM86_PT || in->pred_not || (found >= 0 && (u32)found != k))
            ok = 0;
        else
            found = (int)k;
    }
    free(stack);
    free(seen);
    return ok ? found : -1;
}
#undef BRX_PREDS
/* The load whose table the BRX at i indexes: the instruction that writes the branch's register on every path, or the
   load behind a chain of moves of that value. k, or -1 when no LDC, or ULDC.IDX of a uniform one, reaches it */
static int brx_load(Ctx* c, u32 i, const u32* first, const u32* from) {
    const Sm86Insn* brx = &c->in[i];
    int uniform = brx->op == SM86_BRXU;
    u32 r = sm86_field(brx, 24, uniform ? 6 : 8), at = i;
    for (u32 depth = 0; depth < 8; depth++) {
        int k = brx_writer(c, first, from, at, r, uniform);
        if (k == -2) return -2;
        if (k < 0) return -1;
        const Sm86Insn* w = &c->in[k];
        if (uniform ? (w->op == SM86_ULDC_IDX && sm86_field(w, 73, 3) == 4)
                    : (w->op == SM86_LDC && sm86_field(w, 73, 3) == 4 && !sm86_field(w, 78, 2)))
            return k;
        /* a move of the value, whose source's writer is then the load: what ptxas leaves between the two */
        if (w->op != SM86_MOV && w->op != SM86_UMOV) return -1;
        const Sm86Src* s = &w->b;
        if (s->neg || s->abs) return -1;
        if (s->kind == SM86_SRC_REG && s->reg < SM86_RZ)
            uniform = 0, r = s->reg;
        else if (s->kind == SM86_SRC_UREG && s->reg < SM86_URZ)
            uniform = 1, r = s->reg;
        else
            return -1;
        at = (u32)k;
    }
    return -1;
}
/* The entries of the table of the load at k, as ptxas bounds the index that reaches it: the writer of the load's index
   register on every path, then the writer of that instruction's source, rather than the sixteen instructions before
   the load. 0 when neither the flow nor the scan finds such a bound, and the table is then read until a word is no
   target */
static u32 brx_entries_flow(Ctx* c, const u32* first, const u32* from, u32 k) {
    const Sm86Insn* w = &c->in[k];
    int uniform = w->op == SM86_ULDC_IDX;
    u32 reg = sm86_field(w, 24, uniform ? 6 : 8);
    if (uniform) return uniform_table_entries(c, k, reg);
    u32 entries = 0;
    int m = brx_writer(c, first, from, k, reg, 0);
    if (m >= 0 && jump_table_times_4(&c->in[m])) {
        int b = brx_writer(c, first, from, (u32)m, c->in[m].a.reg, 0);
        if (b >= 0) entries = jump_table_bound_entries(&c->in[b]);
    }
    return entries ? entries : jump_table_entries(c, k, reg); /* the bound beside the load, when the flow has none */
}
/* The jump tables of the program's BRXs, found by the writer of each branch's register that reaches it rather than by
   a scan of the 32 instructions before it: a table loaded far before its BRX, or across a branch, is the one whose
   load reaches it on every path. A table is found from the program's start on, and only before the first BRX whose
   table isn't: past that one, a path may arrive from anywhere. A BRX that the flow can't explain is refused, not given
   the table of one of its paths (a switch on another path's table jumps where the console does not). */
static int brx_resolve(Ctx* c) {
    if (c->brx_resolved) return 1;
    c->brx_table = calloc(c->n ? c->n : 1, sizeof *c->brx_table);
    if (!c->brx_table) return 0;
    c->brx_resolved = 1;
    for (;;) {
        u32 at = c->n;
        for (u32 i = 0; i < c->n; i++)
            if (is_brx(c->in[i].op) && !sm86_never(&c->in[i]) && !c->brx_table[i].state) {
                at = i;
                break;
            }
        if (at == c->n) break;
        u32* from = NULL;
        u32* first = brx_preds(c, &from);
        if (!first) return 0; /* out of memory */
        int load = brx_load(c, at, first, from);
        u32 entries = load >= 0 && g_shader_cb_read ? brx_entries_flow(c, first, from, (u32)load) : 0;
        if (load == -2) {
            free(first);
            return 0; /* out of memory */
        }
        u32 words[64], count = 0;
        if (load >= 0 && g_shader_cb_read)
            count = brx_read_words(c, (u32)load, brx_bias(&c->in[at], at), entries, words, 64, first, from);
        free(first);
        if (!count) {
            /* the flow names no single table load: the scan of the block gives the reason when that is the one, and
               the scan's own table is not used, which may be another path's */
            u32 scan[64];
            if (brx_targets_scan(c, at, scan, 64) && !c->failed)
                fail(c, "BRX whose jump table load doesn't reach it on every path");
            c->brx_table[at].state = 2;
            return 1;
        }
        u32* targets = malloc((size_t)count * sizeof *targets);
        if (!targets) return 0; /* out of memory */
        memcpy(targets, words, (size_t)count * sizeof *targets);
        c->brx_table[at].targets = targets;
        c->brx_table[at].count = count;
        c->brx_table[at].state = 1;
    }
    return 1;
}
/* The targets of the BRX at i from a scan of the 32 instructions before it, as brx_targets found them before the flow
   was searched: the jump table of the LDC that writes its register, read as brx_read_table does. The count, or 0. It
   gives brx_resolve the reason that a BRX has no table, and is the answer for a context that was never resolved */
static u32 brx_targets_scan(Ctx* c, u32 i, u32* targets, u32 max) {
    const Sm86Insn* in = &c->in[i];
    if (!(in->pred == SM86_PT && !in->pred_not)) {
        fail(c, "BRX on a predicate");
        return 0;
    }
    if (!g_shader_cb_read) {
        fail(c, "BRX without a reader of constant buffers");
        return 0;
    }
    int k = brx_scan_load(c, i);
    if (k >= 0) return brx_read_table(c, (u32)k, brx_bias(in, i), targets, max);
    fail(c, "BRX without the LDC of its jump table");
    return 0;
}
/* The targets of the BRX at i: the table that brx_resolve found, else the scan of its block, which is only for a
   context that was never resolved (c->failed is already set when the flow refused a table) */
static u32 brx_targets(Ctx* c, u32 i, u32* targets, u32 max) {
    if (c->brx_table && c->brx_table[i].state) {
        u32 count = c->brx_table[i].count;
        if (!count || count > max) return 0;
        memcpy(targets, c->brx_table[i].targets, (size_t)count * sizeof *targets);
        return count;
    }
    return brx_targets_scan(c, i, targets, max);
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
    c->nblocks = nb;
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
        /* ptxas leaves a loop with @P CALL.REL.NOINC to the code after the loop's back edge, an unconditional BRA
           backwards right after the CALL: the loop's exit, a branch. The search below for the RETs up to the next BRA
           to itself goes on past the program's last EXIT into the functions after it, and took such an exit for a
           call of everything from there on: copied for each exit, the copies' own exits copied again, a vertex
           program of 19,584 instructions went past 65,536 and a compute program past 64 copies */
        if (entry == i + 2 && i + 1 < n && out[i + 1].op == SM86_BRA && sm86_always(&out[i + 1]) &&
            sm86_branch_target(&out[i + 1], i + 1) <= i) {
            out[i] = make_bra(&out[i], i, entry);
            continue;
        }
        while (end < n &&
               !(out[end].op == SM86_BRA && sm86_always(&out[end]) && sm86_branch_target(&out[end], end) == end))
            end++;
        if (entry >= *count || end >= *count || ++copies > 64) {
            fail(c, "CALL to 0x%x that the inlining can't take", entry * 16);
            free(out);
            return NULL;
        }
        /* and a CALL to code that no RET ends before the next BRA to itself: a branch too */
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
        case SM86_LDG_NOU: return mem_words(type);
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
            case SM86_PLOP3:
            case SM86_PSETP: return !u && (p81 || p84);
            case SM86_UPLOP3: return u && (p81 || p84);
            case SM86_LEA:
            case SM86_ULEA:
            case SM86_IMAD_WIDE: /* their carry out */
            case SM86_UIMAD_WIDE:
            case SM86_IMAD_HI:
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
            case SM86_TXD:
            case SM86_TXD_B:
            case SM86_TXD_U:
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
            if (sm86_field(in, 74, 1) && !mh_pred(c, k, u, 87)) return 0; /* .X: the carry predicate joins the sum */
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
    if (!tex_table_slot(c, i, type, array, shadow, ms)) {
        c->tex[i] =
            spv_emit_sampler_image(&c->e, c->t_f32, (u32)shader_vk_tex_set(c->stage), SNX_VKB_TEX + (u32)i,
                                   type == 0 ? 1 : type, (int)array, (int)shadow, ms ? 1 : 0, &c->tex_sampled[i]);
        add_interface(c, c->tex[i]);
    }
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
/* the handle register of a TEX.B, or of a TXQ.B: its first source, the level in the register after it (NAK's OpTxq
   with TexRef::Bindless) */
static u32 handle_b_reg(const Sm86Insn* in) {
    return in->op == SM86_TXQ_B ? sm86_field(in, 24, 8) : tex_b_handle_reg(in);
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
    /* a compute program has no texture arrays, so a table's LDC (its address has a register) is tried as a handle
       from memory: where every lane picks the same element, the dispatch's own, it is that texture; else none */
    if (c->stage == ST_CS && w->op == SM86_LDC && sm86_field(w, 24, 8) < SM86_RZ) return 0;
    return (w->op == SM86_MOV && w->b.kind == SM86_SRC_CBUF && !w->b.bindless) || w->op == SM86_LDC;
}
/* The handles from memory: TEX, TLD, TLD4, TXD and TXQ with the handle in a uniform register pair (URn the texture's
   word, URn + 1 the sampler's), and TEX.B whose handle register no constant buffer word gives. Their slots go in
   c->handle (the slot + 1), and the instructions that make them in ShaderInfo.memh */
static void memory_handles(Ctx* c) {
    u32 n = c->n, any = 0;
    for (u32 i = 0; i < n; i++) {
        int op = c->in[i].op;
        any |= op == SM86_TEX_U || op == SM86_TLD_U || op == SM86_TLD4_U || op == SM86_TXD_U || op == SM86_TXQ_U ||
               ((op == SM86_TEX_B || op == SM86_TXQ_B) && handle_b_reg(&c->in[i]) < SM86_RZ &&
                !tex_b_cbuf_handle(c, i, handle_b_reg(&c->in[i])));
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
        int op = in->op, u = op == SM86_TEX_U || op == SM86_TLD_U || op == SM86_TLD4_U || op == SM86_TXQ_U ||
                             op == SM86_TXD_U;
        int b = op == SM86_TEX_B || op == SM86_TXQ_B;
        u32 reg = b ? handle_b_reg(in) : SM86_RZ;
        if (!u && !(b && reg < SM86_RZ && !tex_b_cbuf_handle(c, i, reg))) continue;
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
            int query = op == SM86_TXQ_U || op == SM86_TXQ_B;
            if (probe[i][0] == ~0u || query != (pass == 1)) continue;
            if (op != SM86_TEX_U && op != SM86_TLD_U && op != SM86_TLD4_U && op != SM86_TXD_U && !query &&
                op != SM86_TEX_B)
                continue;
            u32 tex = mh_final(c, probe[i][0]), smp = probe[i][1] == ~0u ? 0 : mh_final(c, probe[i][1]);
            int has_smp = probe[i][1] != ~0u;
            if (query) {
                for (int k = 0; k < c->si->ntex && slot < 0; k++) {
                    const ShTex* t = &c->si->tex[k];
                    if ((t->flags & SNX_TEX_MEMORY) && t->cbuf_off == tex && t->bl2 == has_smp && t->bl2_off == smp)
                        slot = k;
                }
                if (slot < 0) slot = tex_slot_memory(c, tex, has_smp, smp, 1, 0, 0, 0);
            } else {
                u32 type, array, coords;
                if (!tex_dim(sm86_field(in, 61, 3), &type, &array, &coords)) continue; /* refused where it's written */
                /* TXD has no depth reference: its bit 78 is not the shadow bit */
                u32 shadow = op == SM86_TLD_U || op == SM86_TXD_U ? 0 : sm86_field(in, 78, 1),
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
/* The instruction that makes the address register of a table's LDC, x * stride + base by constants: an IMAD without
   .X; the low word of a left SHF by a constant (x << s: stride 1 << s, base 0), as another game's material and light
   tables are indexed, 4, 8 and 16 bytes apart; or a LEA of the low word, without the negation or the carry in
   ((x << s) + base, its b a constant). 0 where it is none of them */
static int handle_table_index(Ctx* c, int m, u32* stride, u32* base) {
    if (m < 0) return 0;
    const Sm86Insn* w = &c->in[m];
    u32 s = 0;
    *stride = 0, *base = 0;
    if (w->op == SM86_IMAD) {
        if (sm86_field(w, 74, 1) || !src_const(c, (u32)m, &w->b, stride) || !src_const(c, (u32)m, &w->c, base))
            return 0;
    } else if (w->op == SM86_SHF || w->op == SM86_USHF) {
        /* bit 76: right; bit 80: the high word. Under 32, the low word is the same for a 64-bit shift */
        if (sm86_field(w, 76, 1) || sm86_field(w, 80, 1) || !src_const(c, (u32)m, &w->b, &s) || s < 2 || s > 15)
            return 0;
        *stride = 1u << s;
    } else if (w->op == SM86_LEA) {
        /* bit 72: the negation; bit 74: .X; bit 80: .HI. A carry out predicate changes nothing of the sum */
        s = sm86_field(w, 75, 5);
        if (sm86_field(w, 72, 1) || sm86_field(w, 74, 1) || sm86_field(w, 80, 1) || s < 2 || s > 15 ||
            !src_const(c, (u32)m, &w->b, base))
            return 0;
        *stride = 1u << s;
    } else
        return 0;
    return *stride && *stride <= 0xffff;
}
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
                if (!g_spirv_translate.texture_arrays || c->stage == ST_CS) {
                    /* a compute program's table doesn't come here: tex_b_cbuf_handle leaves its TEX.B to
                       memory_handles, which gives it a slot (the dispatch's element, or none) or fails the program */
                    fail(c, "TEX.B of a table of handles, without texture arrays");
                } else if (!handle_table_index(c, m, &stride, &base)) {
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
    if (m->payload_buf) {
        /* the compute-task path: one stride for both programs' slots, and the counts and record before them */
        if (!m->payload_stride || m->payload_stride < c->payload_words) {
            fail(c, "a compute-task slot of %u words for a payload of %u", m->payload_stride, c->payload_words);
            return;
        }
        c->payload_buf = 1;
        c->payload_stride = m->payload_stride;
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

/* A complementary pair: a guarded write of register R, then another of R under the opposite guard of the same
   predicate, @!P R = a ... @P R = b (or the reverse), which the compiler makes of a SEL. As two guarded writes, each
   in its own selection, an optimizer folds them into select(P, b, select(!P, a, R)), and NVIDIA's compiler (621.24)
   made of one game's vertex program with such a pair code that faulted the GPU at an invalid instruction pointer
   (and crashed in its compiler on variants of it). As one select of the two values, R = P ? b : a, the program
   computes the same: the halves are ALU writes of R alone, which may run without their guard. The instructions
   between may not read or write R, nor write P, and are ALU writes of their own that read only their a, b and c */
static int pair_op(const Sm86Insn* in) {
    switch (in->op) {
        case SM86_MOV:
        case SM86_FADD:
        case SM86_FMUL:
        case SM86_FFMA:
        case SM86_SEL:
        case SM86_FSEL:
        case SM86_IMAD:
        case SM86_IABS: return 1;
        default: return 0;
    }
}
/* whether one of the sources the instruction reads (the decoder fills a and c for every ALU form) is register r */
static int pair_reads(const Sm86Insn* in, u32 r) {
    const Sm86Src* used[3] = {&in->a, &in->b, &in->c};
    switch (in->op) {
        case SM86_MOV:
        case SM86_IABS: used[0] = used[2] = NULL; break;
        case SM86_FMUL:
        case SM86_SEL:
        case SM86_FSEL: used[2] = NULL; break;
        case SM86_FADD:
            used[1] = second(in);
            used[2] = NULL;
            break;
        default: break; /* FFMA, IMAD, IADD3, LOP3: all three */
    }
    for (int k = 0; k < 3; k++)
        if (used[k] && used[k]->kind == SM86_SRC_REG && used[k]->reg == r) return 1;
    return 0;
}
/* the instruction that completes the pair the guarded instruction i starts, in the body that ends at end; 0 for none */
static u32 pair_partner(const Sm86Insn* insn, u32 i, u32 end) {
    const Sm86Insn* a = &insn[i];
    if (!pair_op(a) || a->pred >= SM86_PT || sm86_has_cond(a) || a->dst >= SM86_RZ) return 0;
    for (u32 k = i + 1; k < end && k <= i + 32; k++) {
        const Sm86Insn* b = &insn[k];
        if (sm86_never(b)) continue;
        if (pair_op(b) && b->dst == a->dst && b->pred == a->pred && b->pred_not != a->pred_not && !sm86_has_cond(b))
            return k;
        if (!(pair_op(b) || b->op == SM86_IADD3 || b->op == SM86_LOP3) || sm86_has_cond(b) || may_write(b, a->dst) ||
            pair_reads(b, a->dst) || mh_may_write(b, MH_P, a->pred))
            return 0;
    }
    return 0;
}
/* the value instruction i writes to its destination, computed where it stands but not stored */
static u32 pair_value(Ctx* c, const Sm86Insn* in, u32 i) {
    c->cap_on = 1;
    c->cap_reg = in->dst;
    c->cap_n = 0;
    c->cur = i;
    emit_insn(c, in);
    c->cap_on = 0;
    if (c->cap_n != 1 && !c->failed)
        fail(c, "%s wrote R%u %u times in a pair", sm86_ops[in->op].name, in->dst, c->cap_n);
    return c->cap_val;
}
u32* sm86_to_spirv(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count) {
    return sm86_to_spirv_ftz(code, bytes, stage, sph, si, word_count, SH_FTZ_EMULATE, SM86_LANES_WARP);
}
u32* sm86_to_spirv_ftz(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count,
                       int can_flush, int lanes) {
    /* the whole SM86 translation, with the clock read only when a client can take the plot */
    int trace = snx_tracy_enter();
    u64 t0 = trace ? plat_ns() : 0;
    if (trace) snx_tracy_leave();
    u64 phase[P86_N] = {0}, h = 0, pt = 0;
    if (!code || !si || !word_count || stage < ST_VS || stage > ST_MS || bytes < 16) return NULL;
    if (stage != ST_CS && !sph) return NULL;
    if (stage == ST_TCS && sm86_tcs_empty(sph[2])) {
        si->unsupported = 1;
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "a tessellation control program with no invocation (the passthrough takes its place)");
        SNX_MSG(si->first_unsupported, strlen(si->first_unsupported));
        SNX_PLOT("shader translate failures", 1);
        return NULL;
    }
    if (stage != ST_VS && stage != ST_GS && stage != ST_FS && stage != ST_CS && stage != ST_TS && stage != ST_MS) {
        si->unsupported++;
        snprintf(si->first_unsupported, sizeof si->first_unsupported, "stage %d", stage);
        SNX_MSG(si->first_unsupported, strlen(si->first_unsupported));
        SNX_PLOT("shader translate failures", 1);
        sm86_debug_dump("program", code, bytes, sph, si->first_unsupported);
        return NULL;
    }
    u32 n = bytes / 16;
    SNX_ZONE_TV(z_snx_sm86, "shader translate sm86", n, "stage %d, %u insn", stage, n);
    if (trace) {
        h = snx_fnv1a(SNX_FNV_BASIS, code, bytes);
        SNX_ZONE_TEXT(z_snx_sm86, "stage %d, %u insn, hash %016llx", stage, n, (unsigned long long)h);
        (void)h; /* the text's arguments are dropped without a client */
    }
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
    c->lanes = lanes;
    c->sph = sph ? sph : zero_sph;
    c->in = insn;
    c->n = n;
    si->stage = stage;
    if (sph)
        for (u32 i = 0; i < 20; i++) si->sph[i] = sph[i];
    if (stage == ST_TS || stage == ST_MS) mesh_layout(c);
    if (stage == ST_GS) {
        /* the input primitive (the renderer's key), and the SPH's passthrough (word 0 bit 24), output topology (word
           3, bits 24-27), output vertices (word 4, bits 0-11) and invocations (word 2, bits 24-31), checked as
           spirv_translate.c checks Maxwell's */
        u32 vertices = gs_vertices(c), topology = (sph[3] >> 24) & 15;
        c->passthrough = sph[0] >> 24 & 1;
        if (vertices != 1 && vertices != 2 && vertices != 3 && vertices != 4 && vertices != 6)
            fail(c, "a geometry input primitive of %u vertices", vertices);
        else if (!c->passthrough && topology != 1 && topology != 6 && topology != 7)
            fail(c, "geometry output topology %u", topology);
        else if ((sph[4] & 0xfff) > 256 || (sph[2] >> 24) > 32)
            fail(c, "%u geometry output vertices or %u invocations", sph[4] & 0xfff, sph[2] >> 24);
    }
    pt = trace ? plat_ns() : 0;
    SNX_ZONE(z_snx_decode, "shader decode");
    sm86_load(code, n, words);
    u32 top = 0;
    for (u32 i = 0; i < n; i++) {
        if (!sm86_decode(words[2 * i], words[2 * i + 1], &insn[i]))
            fail(c, "unknown opcode 0x%03x", (u32)(words[2 * i] & 0xfff));
        else if (!stage_ok(c, &insn[i]))
            fail(c, "%s in stage %d", sm86_ops[insn[i].op].name, stage);
        if (!c->failed && (is_brx(insn[i].op) || insn[i].op == SM86_BRA)) c->nbranches++;
        u32 r = max_register(&insn[i]);
        if (r > top) top = r;
    }
    SNX_ZONE_END(z_snx_decode);
    if (trace) phase[P86_DECODE] += plat_ns() - pt;
    SNX_PLOT("shader instructions", (double)n);
    SNX_PLOT("shader branches", (double)c->nbranches);
    pt = trace ? plat_ns() : 0;
    SNX_ZONE(z_snx_dataflow, "shader dataflow");
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
    /* after the CALLs are inlined and before the passes that move instructions: the pairs of instructions that one
       operation takes the place of (fuse.c), whose spans the array moves invalidate. A pair whose second half a jump
       lands on (a BRA's target, a BRX's case, flow_sources' edges) stays two, as that jump runs the second alone */
    /* a lone lane's proof reads what each instruction reads and writes of itself: no pair of two, no fused ones (notes) */
    if (!c->failed) lane_gate(c);
    if (!c->notes) {
        u32 nfuse = 0, kept = 0;
        FuseSpan* fuse = sm86_fuse_find(insn, n, &nfuse);
        if (fuse && !flow_sources(c)) fail(c, "out of memory");
        for (u32 s = 0; fuse && s < nfuse && !c->failed; s++) {
            u32 second = fuse[s].pos + 1;
            if (c->jump_first[second] == c->jump_first[second + 1]) fuse[kept++] = fuse[s];
        }
        sm86_fuse_make(insn, n, fuse, c->failed ? 0 : kept);
    }
    if (!c->failed && !track_globals(c)) fail(c, "out of memory");
    SNX_ZONE_END(z_snx_dataflow);
    if (trace) phase[P86_DATAFLOW] += plat_ns() - pt;
    pt = trace ? plat_ns() : 0;
    SNX_ZONE(z_snx_structure, "shader structure");
    ShStructure* st = c->failed ? NULL : build_structure(c);
    SNX_ZONE_TEXT(z_snx_structure, "%s", st ? "structured" : "no structure");
    if (!st && c->si->first_unsupported[0]) SNX_ZONE_TEXT(z_snx_structure, "why: %.80s", c->si->first_unsupported);
    SNX_ZONE_VALUE(z_snx_structure, st ? 1 : 0); /* 1 structured, 0 the dispatch loop is needed */
    SNX_ZONE_END(z_snx_structure);
    if (trace) phase[P86_STRUCTURE] += plat_ns() - pt;
    SNX_PLOT("shader blocks", (double)c->nblocks);
    u32* out = NULL;
    if (!c->failed && st) {
        SpvEmitter* e = &c->e;
        pt = trace ? plat_ns() : 0;
        SNX_ZONE(z_snx_alloc, "shader alloc");
        spv_emit_init(e, (size_t)n * 40 + 512);
        spv_emit_shader_capability(e);
        /* hmma_coop's matrices, which SPV_KHR_cooperative_matrix allows in a module of the Vulkan memory model only */
        c->coop = coop_wanted(c);
        /* the u16 view is written only by 32-bit shared stores: a program with a shared atomic or a sub-word
           shared store cannot keep it coherent, so such a program takes the general LDSM path */
        if (g_spirv_translate.hmma_narrow_lds)
            for (u32 i = 0; i < n && !c->narrow_unsafe; i++) {
                const Sm86Insn* q = &insn[i];
                if (q->op == SM86_ATOMS || q->op == SM86_ATOMS_NOU || q->op == SM86_ATOMS_CAS ||
                    q->op == SM86_ATOMS_POPC_INC ||
                    ((q->op == SM86_STS || q->op == SM86_STS_NOU) && sm86_field(q, 73, 3) < 4))
                    c->narrow_unsafe = 1;
            }
        if (c->coop) {
            e->vulkan_memory_model = 1;
            spv_emit_capability(e, SPV_CAPABILITY_VULKAN_MEMORY_MODEL);
            spv_emit_capability(e, SPV_CAPABILITY_VULKAN_MEMORY_MODEL_DEVICE_SCOPE);
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
        coop_label(c, spv_emit_id(e));
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
        for (u32 i = 0; i < n && !c->cas_var && !g_spirv_translate.float_atomic_add; i++)
            if ((insn[i].op == SM86_RED || insn[i].op == SM86_ATOMG || insn[i].op == SM86_ATOMG_NOU) &&
                sm86_field(&insn[i], 73, 3) == 3)
                c->cas_var = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        for (u32 p = 0; p < SM86_PT; p++) {
            c->pred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
            c->upred[p] = spv_emit_function_variable_init(e, pred_ptr, c->false_v);
        }
        prepare_handles(c);
        if (stage == ST_GS) {
            spv_emit_capability(e, SPV_CAPABILITY_GEOMETRY);
            c->viewport_mask = spv_emit_function_variable_init(e, reg_ptr, cu(c, 1));
        }
        /* a vertex program's outputs, made before its code: every exit's depth fix sees the position, and each
           starts as (0, 0, 0, 1), which the console gives the components a program doesn't write (spirv_translate.c
           does the same for Maxwell's). One game's full screen passes write only the position's x and y, and with
           an undefined w their triangles drew nothing */
        for (u32 i = 0; i < n && stage == ST_VS; i++)
            if (insn[i].op == SM86_AST)
                for (u32 k = 0; k <= sm86_field(&insn[i], 74, 2); k++)
                    output_var(c, sm86_field(&insn[i], 40, 10) + 4 * k);
        /* and the clip distances of a vertex or geometry program, when it stores one that is used. uses_clip_mask names
           those planes, as spirv_translate.c's clip_used does: the renderer gives such a program no varying geometry
           program of its own (render.c), and a tessellation passthrough hands them on (tess_passthrough.c) */
        for (u32 i = 0; i < n && (stage == ST_VS || stage == ST_GS); i++)
            if (insn[i].op == SM86_AST)
                for (u32 k = 0; k <= sm86_field(&insn[i], 74, 2); k++) {
                    u32 addr = sm86_field(&insn[i], 40, 10) + 4 * k;
                    if (addr < 0x2c0 || addr >= 0x2e0 || !clip_plane_used(c, (addr - 0x2c0) >> 2)) continue;
                    c->si->uses_clip_mask |= 1 << ((addr - 0x2c0) >> 2);
                    clip_var(c);
                }
        StructEmit se = {0};
        se.st = st;
        se.open = 1;
        se.label = calloc(4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1, sizeof *se.label + 1);
        if (!se.label) abort();
        se.used = (u8*)(se.label + 4 * (size_t)st->nscopes + 2 * (size_t)st->ntokens + 1);
        if (st->uses_flag) se.flag = spv_emit_function_variable_init(e, reg_ptr, c->zero);
        /* a vertex program's own attribute fetch, after the other Function variables: its code begins the block */
        vertex_fetch_prologue(c);
        /* pair_partner's pairs: the first half's value, kept for the second, which writes the select */
        u32* pair_first = calloc((size_t)n + 1, sizeof *pair_first);
        u32* pair_value_of = calloc((size_t)n + 1, sizeof *pair_value_of);
        if (!pair_first || !pair_value_of) abort();
        if (stage == ST_VS) {
            u32 parts[4] = {cf(c, 0.0f), cf(c, 0.0f), cf(c, 0.0f), cf(c, 1.0f)};
            u32 empty = spv_emit_constant_vec4(e, c->t_v4f, parts);
            if (c->position) spv_emit_store(e, c->position, empty);
            for (u32 i = 0; i < 32; i++)
                if (c->out_attr[i]) spv_emit_store(e, c->out_attr[i], empty);
        }
        for (u32 plane = 0; plane < 8 && c->clip; plane++)
            spv_emit_store(e, spv_emit_output_element_pointer(e, c->t_f32, c->clip, cu(c, plane)), cf(c, 0.0f));
        SNX_ZONE_END(z_snx_alloc);
        if (trace) phase[P86_ALLOC] += plat_ns() - pt;
        pt = trace ? plat_ns() : 0;
        SNX_ZONE(z_snx_emit, "shader emission");
        while (!c->failed) {
            int step = structure_step(c, &se);
            if (step == SE_DONE) break;
            if (step == SE_END) {
                epilogue(c);
                coop_end(c);
                se.open = 0;
                continue;
            }
            for (u32 i = se.body_start; i < se.body_end && !c->failed; i++) {
                const Sm86Insn* in = &insn[i];
                int always = sm86_always(in), never = sm86_never(in);
                if (c->notes) c->notes->at = i + 1;
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
                    coop_end(c);
                    se.open = 0;
                    break;
                }
                if (!always && pair_first[i]) { /* a pair's second half: R = its guard ? its value : the first's */
                    u32 b = pair_value(c, in, i);
                    if (!c->failed)
                        wr(c, in->dst, spv_emit_ternary(e, SPV_OP_SELECT, c->t_u32, guard(c, in), b, pair_value_of[i]));
                    continue;
                }
                if (!always && in->op != SM86_EXIT && !c->notes) {
                    u32 j = pair_partner(insn, i, se.body_end);
                    if (j) {
                        pair_value_of[j] = pair_value(c, in, i);
                        pair_first[j] = i + 1;
                        continue;
                    }
                }
                u32 merge = 0;
                if (!always) {
                    u32 cond = guard(c, in), then = spv_emit_id(e);
                    merge = spv_emit_id(e);
                    coop_merge(c, merge);
                    coop_branch_if(c, cond, then, merge);
                    coop_label(c, then);
                }
                if (in->op == SM86_EXIT) {
                    epilogue(c);
                    coop_end(c);
                } else {
                    c->cur = i;
                    emit_insn(c, in);
                    if (merge) coop_branch(c, merge);
                }
                if (merge) coop_label(c, merge);
            }
        }
        SNX_ZONE_END(z_snx_emit);
        if (trace) phase[P86_EMIT] += plat_ns() - pt;
        if (se.open && !c->failed) {
            epilogue(c);
            coop_end(c);
        }
        spv_emit_function_end(e);
        free(pair_first);
        free(pair_value_of);
        free(se.label);
        int mesh = stage == ST_TS || stage == ST_MS;
        /* the compute-task path: a task program is a compute program, and the mesh program it launches has no task
           stage (sm86.h's sm86_mesh_task_cap) */
        int task_compute = stage == ST_TS && c->payload_buf;
        if (!c->failed && mesh) {
            /* the program is the body of an entry point of its own */
            if (!task_compute) mesh_capability(c);
            fn = mesh_entry(c, fn);
            spv_emit_local_size(e, fn, si->mesh.threads, 1, 1);
            if (stage == ST_MS) {
                spv_emit_execution_mode(e, fn, SPV_MODE_OUTPUT_VERTICES, 1, si->mesh.max_vertices);
                spv_emit_execution_mode(e, fn, SPV_MODE_OUTPUT_PRIMITIVES_EXT, 1, si->mesh.max_primitives);
                spv_emit_execution_mode(e, fn, SPV_MODE_OUTPUT_TRIANGLES_EXT, 0, 0);
            }
        }
        if (!c->failed) {
            SpvShaderStage model = task_compute ? SPV_STAGE_COMPUTE
                                   : stage == ST_TS   ? SPV_STAGE_TASK
                                   : stage == ST_MS   ? SPV_STAGE_MESH
                                                      : (SpvShaderStage)stage;
            spv_emit_entry_str(e, model, fn, "main", c->interfaces, c->ninterfaces);
            if (stage == ST_GS) {
                /* the input primitive, the output topology (a passthrough program's the input's) and vertices, and
                   the invocations of each input primitive, as spirv_translate.c gives Maxwell's */
                u32 vertices = gs_vertices(c), topology = (c->sph[3] >> 24) & 15, invocations = c->sph[2] >> 24;
                if (c->passthrough) topology = vertices == 1 ? 1 : vertices == 2 || vertices == 4 ? 6 : 7;
                spv_emit_execution_mode(e, fn,
                                        vertices == 1   ? 19
                                        : vertices == 2 ? 20
                                        : vertices == 4 ? 21
                                        : vertices == 6 ? 23
                                                        : 22,
                                        0, 0);
                spv_emit_execution_mode(e, fn, topology == 1 ? 27 : topology == 6 ? 28 : 29, 0, 0);
                spv_emit_execution_mode(e, fn, SPV_MODE_OUTPUT_VERTICES, 1,
                                        c->passthrough         ? (vertices == 4 ? 2 : vertices == 6 ? 3 : vertices)
                                        : c->sph[4] & 0xfff ? c->sph[4] & 0xfff
                                                            : 1);
                spv_emit_execution_mode(e, fn, SPV_MODE_INVOCATIONS, 1, invocations ? invocations : 1);
            }
            if (stage == ST_FS) spv_emit_fragment_origin_upper_left(e, fn);
            /* the depth and stencil tests before the program where its key asks for them (DsPrograms.early_z), as
               spirv_translate.c gives Maxwell's: without them a fragment the tests reject still runs the program's
               image and memory stores */
            if (stage == ST_FS && si->early_fragment_tests)
                spv_emit_execution_mode(e, fn, SPV_MODE_EARLY_FRAGMENT_TESTS, 0, 0);
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
    if (out && c->notes) {
        /* the lone lane's answer, or the reason it is none: the words were written for nothing */
        char why[sizeof si->first_unsupported];
        if (!sm86_lone_gives(insn, n, &c->lone_notes, si, why, sizeof why)) {
            free(out);
            out = NULL;
            *word_count = 0;
            fail(c, "%s", why);
        }
    }
    free(c->lone_notes.word);
    if (!out && si->unsupported) sm86_debug_dump("program", code, bytes, sph, si->first_unsupported);
    free(st);
    free(c->addr_key);
    free(c->fused);
    free(c->handle);
    free(c->mh_mark);
    free(c->mh_pos);
    free(c->mh_dep);
    free(c->mh_new);
    free(c->jump_first);
    if (c->brx_table) {
        for (u32 i = 0; i < c->n; i++) free(c->brx_table[i].targets);
        free(c->brx_table);
    }
    free(insn);
    free(words);
    free(c);
    if (trace) {
        int dom = 0;
        for (int i = 1; i < P86_N; i++)
            if (phase[i] > phase[dom]) dom = i;
        SNX_ZONE_VALUE(z_snx_sm86, dom + 1); /* 1 decode, 2 dataflow, 3 structure, 4 alloc, 5 emit */
        SNX_ZONE_TEXT(z_snx_sm86, "%u spv words, hash %016llx, %s, %s %llu us", out ? (unsigned)*word_count : 0u,
                      (unsigned long long)h, out ? "made" : "rejected", sm86_phase_name[dom],
                      (unsigned long long)(phase[dom] / 1000));
        if (!out && si->first_unsupported[0]) SNX_ZONE_TEXT(z_snx_sm86, "rejected: %.80s", si->first_unsupported);
        SNX_PLOT("shader sm86 translate us", (double)(plat_ns() - t0) / 1000.0);
    }
    return out;
}
