/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Direct Maxwell to SPIR-V translation. Unsupported instructions reject the entire module. to_spirv reads the program
   (the prescan: what it uses, its resources, its flow), declares the module's types, interface and resources, and
   writes each instruction through the emit_ file of its family (translate.h's Tr carries what they read) */
#include "nx/gpu/shader/translate.h"

int g_shader_texture_arrays;
int g_shader_stage_point_size;
int g_shader_output_layer = 1;
int g_shader_viewport_index = 3;
int g_shader_geometry = 1;
int g_shader_float16 = 1;
int g_shader_draw_parameters = 1;
int g_shader_quad = 1;
int g_shader_tessellation = 1;
int g_shader_clock_scopes;
int g_shader_maximal_reconvergence;

/* the family of instructions whose branches take each opcode, FAM_NONE for one no branch takes (refused by
   the prescan, or one that writes nothing): a family's function is the only one with branches for its opcodes */
enum { FAM_NONE, FAM_FLOW, FAM_IO, FAM_THREAD, FAM_INT, FAM_FLOAT, FAM_MEMORY, FAM_TEXTURE };
static const u8 op_family[O__COUNT] = {
    [O_BRA] = FAM_FLOW,        [O_BRX] = FAM_FLOW,         [O_SSY] = FAM_FLOW,       [O_SYNC] = FAM_FLOW,
    [O_PBK] = FAM_FLOW,        [O_BRK] = FAM_FLOW,         [O_PCNT] = FAM_FLOW,      [O_CONT] = FAM_FLOW,
    [O_EXIT] = FAM_FLOW,       [O_KIL] = FAM_FLOW,         [O_CAL] = FAM_FLOW,       [O_RET] = FAM_FLOW,
    [O_PRET] = FAM_FLOW,       [O_PEXIT] = FAM_FLOW,       [O_IDE] = FAM_FLOW,       [O_CCTL] = FAM_FLOW,
    [O_CCTLL] = FAM_FLOW,      [O_BPT] = FAM_FLOW,         [O_IPA] = FAM_IO,         [O_ALD] = FAM_IO,
    [O_AST] = FAM_IO,          [O_OUT_R] = FAM_IO,         [O_OUT_C] = FAM_IO,       [O_OUT_I] = FAM_IO,
    [O_ISBERD] = FAM_IO,       [O_AL2P] = FAM_IO,          [O_PIXLD] = FAM_IO,       [O_S2R] = FAM_THREAD,
    [O_CS2R] = FAM_THREAD,     [O_BAR] = FAM_THREAD,       [O_MEMBAR] = FAM_THREAD,  [O_VOTE] = FAM_THREAD,
    [O_SHFL] = FAM_THREAD,     [O_I2I_R] = FAM_INT,        [O_I2I_C] = FAM_INT,      [O_I2I_I] = FAM_INT,
    [O_MOV_R] = FAM_INT,       [O_MOV_C] = FAM_INT,        [O_MOV_I] = FAM_INT,      [O_MOV32I] = FAM_INT,
    [O_IADD_R] = FAM_INT,      [O_IADD_C] = FAM_INT,       [O_IADD_I] = FAM_INT,     [O_IADD32I] = FAM_INT,
    [O_IADD3_R] = FAM_INT,     [O_IADD3_C] = FAM_INT,      [O_IADD3_I] = FAM_INT,    [O_ISCADD_R] = FAM_INT,
    [O_ISCADD_C] = FAM_INT,    [O_ISCADD_I] = FAM_INT,     [O_ISCADD32I] = FAM_INT,  [O_IMNMX_R] = FAM_INT,
    [O_IMNMX_C] = FAM_INT,     [O_IMNMX_I] = FAM_INT,      [O_ISETP_R] = FAM_INT,    [O_ISETP_C] = FAM_INT,
    [O_ISETP_I] = FAM_INT,     [O_ISET_R] = FAM_INT,       [O_ISET_C] = FAM_INT,     [O_ISET_I] = FAM_INT,
    [O_ICMP_R] = FAM_INT,      [O_ICMP_RC] = FAM_INT,      [O_ICMP_CR] = FAM_INT,    [O_ICMP_I] = FAM_INT,
    [O_LOP_R] = FAM_INT,       [O_LOP_C] = FAM_INT,        [O_LOP_I] = FAM_INT,      [O_LOP32I] = FAM_INT,
    [O_LOP3_R] = FAM_INT,      [O_LOP3_C] = FAM_INT,       [O_LOP3_I] = FAM_INT,     [O_SHL_R] = FAM_INT,
    [O_SHL_C] = FAM_INT,       [O_SHL_I] = FAM_INT,        [O_SHR_R] = FAM_INT,      [O_SHR_C] = FAM_INT,
    [O_SHR_I] = FAM_INT,       [O_SHF_LR] = FAM_INT,       [O_SHF_LI] = FAM_INT,     [O_SHF_RR] = FAM_INT,
    [O_SHF_RI] = FAM_INT,      [O_BFE_R] = FAM_INT,        [O_BFE_C] = FAM_INT,      [O_BFE_I] = FAM_INT,
    [O_BFI_R] = FAM_INT,       [O_BFI_RC] = FAM_INT,       [O_BFI_CR] = FAM_INT,     [O_BFI_I] = FAM_INT,
    [O_SEL_R] = FAM_INT,       [O_SEL_C] = FAM_INT,        [O_SEL_I] = FAM_INT,      [O_POPC_R] = FAM_INT,
    [O_POPC_C] = FAM_INT,      [O_POPC_I] = FAM_INT,       [O_FLO_R] = FAM_INT,      [O_FLO_C] = FAM_INT,
    [O_FLO_I] = FAM_INT,       [O_PRMT_R] = FAM_INT,       [O_PRMT_RC] = FAM_INT,    [O_PRMT_CR] = FAM_INT,
    [O_PRMT_I] = FAM_INT,      [O_XMAD_R] = FAM_INT,       [O_XMAD_RC] = FAM_INT,    [O_XMAD_CR] = FAM_INT,
    [O_XMAD_I] = FAM_INT,      [O_IMUL_R] = FAM_INT,       [O_IMUL_C] = FAM_INT,     [O_IMUL_I] = FAM_INT,
    [O_IMUL32I] = FAM_INT,     [O_IMAD_R] = FAM_INT,       [O_IMAD_RC] = FAM_INT,    [O_IMAD_CR] = FAM_INT,
    [O_IMAD_I] = FAM_INT,      [O_IMAD32I] = FAM_INT,      [O_LEA_HR] = FAM_INT,     [O_LEA_HC] = FAM_INT,
    [O_LEA_LR] = FAM_INT,      [O_LEA_LC] = FAM_INT,       [O_LEA_LI] = FAM_INT,     [O_PSETP] = FAM_INT,
    [O_PSET] = FAM_INT,        [O_P2R_R] = FAM_INT,        [O_P2R_C] = FAM_INT,      [O_P2R_I] = FAM_INT,
    [O_R2P_R] = FAM_INT,       [O_R2P_C] = FAM_INT,        [O_R2P_I] = FAM_INT,      [O_CSET] = FAM_INT,
    [O_CSETP] = FAM_INT,       [O_VMNMX] = FAM_INT,        [O_VMAD] = FAM_INT,       [O_VSETP] = FAM_INT,
    [O_VADD] = FAM_INT,        [O_VABSDIFF] = FAM_INT,     [O_VSET] = FAM_INT,       [O_VSHL] = FAM_INT,
    [O_VSHR] = FAM_INT,        [O_FADD_R] = FAM_FLOAT,     [O_FADD_C] = FAM_FLOAT,   [O_FADD_I] = FAM_FLOAT,
    [O_FADD32I] = FAM_FLOAT,   [O_FMUL_R] = FAM_FLOAT,     [O_FMUL_C] = FAM_FLOAT,   [O_FMUL_I] = FAM_FLOAT,
    [O_FMUL32I] = FAM_FLOAT,   [O_FFMA_R] = FAM_FLOAT,     [O_FFMA_RC] = FAM_FLOAT,  [O_FFMA_CR] = FAM_FLOAT,
    [O_FFMA_I] = FAM_FLOAT,    [O_FFMA32I] = FAM_FLOAT,    [O_FMNMX_R] = FAM_FLOAT,  [O_FMNMX_C] = FAM_FLOAT,
    [O_FMNMX_I] = FAM_FLOAT,   [O_FSETP_R] = FAM_FLOAT,    [O_FSETP_C] = FAM_FLOAT,  [O_FSETP_I] = FAM_FLOAT,
    [O_FSET_R] = FAM_FLOAT,    [O_FSET_C] = FAM_FLOAT,     [O_FSET_I] = FAM_FLOAT,   [O_FCMP_R] = FAM_FLOAT,
    [O_FCMP_RC] = FAM_FLOAT,   [O_FCMP_CR] = FAM_FLOAT,    [O_FCMP_I] = FAM_FLOAT,   [O_MUFU] = FAM_FLOAT,
    [O_RRO_R] = FAM_FLOAT,     [O_RRO_C] = FAM_FLOAT,      [O_RRO_I] = FAM_FLOAT,    [O_F2F_R] = FAM_FLOAT,
    [O_F2F_C] = FAM_FLOAT,     [O_F2F_I] = FAM_FLOAT,      [O_F2I_R] = FAM_FLOAT,    [O_F2I_C] = FAM_FLOAT,
    [O_F2I_I] = FAM_FLOAT,     [O_I2F_R] = FAM_FLOAT,      [O_I2F_C] = FAM_FLOAT,    [O_I2F_I] = FAM_FLOAT,
    [O_FSWZADD] = FAM_FLOAT,   [O_FCHK_R] = FAM_FLOAT,     [O_FCHK_C] = FAM_FLOAT,   [O_FCHK_I] = FAM_FLOAT,
    [O_HADD2_R] = FAM_FLOAT,   [O_HADD2_C] = FAM_FLOAT,    [O_HADD2_I] = FAM_FLOAT,  [O_HADD2_32I] = FAM_FLOAT,
    [O_HMUL2_R] = FAM_FLOAT,   [O_HMUL2_C] = FAM_FLOAT,    [O_HMUL2_I] = FAM_FLOAT,  [O_HMUL2_32I] = FAM_FLOAT,
    [O_HFMA2_R] = FAM_FLOAT,   [O_HFMA2_RC] = FAM_FLOAT,   [O_HFMA2_CR] = FAM_FLOAT, [O_HFMA2_I] = FAM_FLOAT,
    [O_HFMA2_32I] = FAM_FLOAT, [O_HSETP2_R] = FAM_FLOAT,   [O_HSETP2_C] = FAM_FLOAT, [O_HSETP2_I] = FAM_FLOAT,
    [O_HSET2_R] = FAM_FLOAT,   [O_HSET2_C] = FAM_FLOAT,    [O_HSET2_I] = FAM_FLOAT,  [O_DADD_R] = FAM_FLOAT,
    [O_DADD_C] = FAM_FLOAT,    [O_DADD_I] = FAM_FLOAT,     [O_DMUL_R] = FAM_FLOAT,   [O_DMUL_C] = FAM_FLOAT,
    [O_DMUL_I] = FAM_FLOAT,    [O_DFMA_R] = FAM_FLOAT,     [O_DFMA_RC] = FAM_FLOAT,  [O_DFMA_CR] = FAM_FLOAT,
    [O_DFMA_I] = FAM_FLOAT,    [O_DSET_R] = FAM_FLOAT,     [O_DSET_C] = FAM_FLOAT,   [O_DSET_I] = FAM_FLOAT,
    [O_DSETP_R] = FAM_FLOAT,   [O_DSETP_C] = FAM_FLOAT,    [O_DSETP_I] = FAM_FLOAT,  [O_DMNMX_R] = FAM_FLOAT,
    [O_DMNMX_C] = FAM_FLOAT,   [O_DMNMX_I] = FAM_FLOAT,    [O_LDC] = FAM_MEMORY,     [O_LDG] = FAM_MEMORY,
    [O_STG] = FAM_MEMORY,      [O_LDS] = FAM_MEMORY,       [O_STS] = FAM_MEMORY,     [O_LDL] = FAM_MEMORY,
    [O_STL] = FAM_MEMORY,      [O_ATOM] = FAM_MEMORY,      [O_ATOMS] = FAM_MEMORY,   [O_RED] = FAM_MEMORY,
    [O_ATOM_CAS] = FAM_MEMORY, [O_ATOMS_CAS] = FAM_MEMORY, [O_TEX] = FAM_TEXTURE,    [O_TEX_B] = FAM_TEXTURE,
    [O_TEXS] = FAM_TEXTURE,    [O_TLD] = FAM_TEXTURE,      [O_TLD_B] = FAM_TEXTURE,  [O_TLDS] = FAM_TEXTURE,
    [O_TLD4] = FAM_TEXTURE,    [O_TLD4_B] = FAM_TEXTURE,   [O_TLD4S] = FAM_TEXTURE,  [O_TXQ] = FAM_TEXTURE,
    [O_TXQ_B] = FAM_TEXTURE,   [O_TMML] = FAM_TEXTURE,     [O_TMML_B] = FAM_TEXTURE, [O_TXD] = FAM_TEXTURE,
    [O_TXD_B] = FAM_TEXTURE,   [O_SULD] = FAM_TEXTURE,     [O_SUST] = FAM_TEXTURE,   [O_SURED] = FAM_TEXTURE,
    [O_SUATOM] = FAM_TEXTURE};

static u32* to_spirv(const u8* code, const u64* insn, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count,
                     int can_flush, int can_preserve, int can_preserve_f64, int can_subgroup, u32 legacy_fs) {
    u32 clip;
    if (!code || !si || !word_count || max_bytes < 96 || stage < ST_VS || stage > ST_CS) return NULL;
    if (stage == ST_CS) {
        ShaderInfo original = *si;
        u32* simple = shader_compute_to_spirv(code, max_bytes, si, word_count, can_flush, can_preserve);
        if (simple) return simple;
        *si = original;
        if (!si->cs_local[0] || !si->cs_local[1] || !si->cs_local[2]) return direct_reject(si, "compute local size");
    }
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    u32 sph[20];
    for (u32 i = 0; i < 20; i++) sph[i] = snx_ld_le32(code + i * 4);
    /* a fragment program's legacy varyings read through generics 17-31 (shader_legacy_varyings) interpolate as the header's
       color map (word 14: the diffuse and specular colors' components, 2 bits each as a generic's) and texture
       coordinate map (bytes 0x3c-0x45, one a coordinate) give them; the back colors and the fog have no map, and
       take the default an unmarked input has */
    for (u32 g = 0; g < 15; g++) {
        u32 generic = LEGACY_GENERIC + g, mode = 0, shift = generic % 4 * 8;
        if (!(legacy_fs >> g & 1)) continue;
        if (g < 2) mode = sph[14] >> (8 * g) & 0xff;
        if (g >= 5) mode = sph[15 + (g - 5) / 4] >> ((g - 5) % 4 * 8) & 0xff;
        sph[6 + generic / 4] = (sph[6 + generic / 4] & ~(0xffu << shift)) | mode << shift;
    }
    u32 target = stage == ST_CS ? 0 : sph[18];
    if (stage == ST_FS && (sph[19] & ~3u)) return direct_reject(si, "fragment output beyond depth and mask");
    /* the sample mask (SPH word 19 bit 0) and depth (bit 1) follow the colours, in that order */
    int passthrough = stage == ST_GS && (sph[0] >> 24 & 1);
    u8 used[255] = {0};
    u8 pred_used[7] = {0};
    u8 inputs_used[32] = {0};
    u8 outputs_used[32] = {0};
    int indexed_store = 0;
    int position_used = 0, layer_output_used = 0, layer_input_used = 0, point_size_used = 0;
    int viewport_used = 0, viewport_mask_one = 0; /* and a viewport mask of viewport 0 written */
    /* the stages between the vertex and fragment ones: their input arrays' length (32 for tessellation, the input
       primitive's vertices for geometry), the control program's output vertices, and what they read and write */
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 in_vertices = stage == ST_GS ? si->rt_int_mask : 32, out_vertices = stage == ST_TCS ? sph[2] >> 24 : 0;
    u8 patch_used[30] = {0};
    int levels_used = 0, position_in_used = 0, primitive_id_used = 0, tess_coord_used = 0, invocation_used = 0;
    int last = stage != ST_TCS && !(si->clip_enable & SNX_SH_NOT_LAST); /* before rasterization */
    u32 clip_used = 0;
    u8 cbuf_used[18] = {0};
    u32 cbuf_max[18] = {0};
    ShTex textures[SH_MAXTEX] = {0};
    int texture_count = 0;
    u64 tex_read = 0, tex_elsewhere = 0; /* slots read, and of them read elsewhere than the fragment's own pixel */
    u64 tex_query = 0;                   /* slots a TXQ alone names: their type is the key's hint, or 2D */
    ShaderInfo image_info = {0};
    int needs_fragcoord = 0;
    int needs_frontface = 0;
    int needs_point_coord = 0, needs_sample_id = 0, needs_coverage = 0;
    int needs_demote = 0;
    int needs_image_query = 0, needs_derivatives = 0, needs_clock = 0;
    int needs_instance = 0;
    int needs_draw_params = 0; /* a vertex program reads NVN's draw parameters (shader.h SH_CB0_) */
    int needs_local_id = 0, needs_group_id = 0;
    int needs_scale = 0, needs_texel_scale = 0, needs_frag_y = 0;
    int needs_glsl = 0, needs_f16 = 0, needs_f64 = 0, needs_i64 = 0;
    int has_flow = 0, has_calls = 0, has_barrier = 0, converge;
    u32 wait_label = 0;
    int first_indexed[SH_INDEXED_TABLES] = {0}, indexed_count = 0, indexed_table[SH_MAXTEX] = {0}, k_indexed;
    int indexed_shadow = 0;
    int has_stack = 0, has_pexit = 0, has_conditional_exit = 0;
    int needs_local = 0, needs_shared = 0, needs_subgroup = 0, needs_atomic_cas = 0, lane_used = 0, needs_quad = 0;
    int needs_gather_extended = 0;
    int fp32_flush = 0, fp32_preserve = 0;
    u32 out_reg = 0;
    if (stage == ST_FS)
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                if (out_reg > 251) return direct_reject(si, "fragment output registers");
                for (int k = 0; k < 4; k++)
                    if (target & (1u << (rt * 4 + k))) used[out_reg + k] = 1;
                out_reg += 4;
            }
    if (stage == ST_FS && (sph[19] & 3u)) {
        if (sph[19] & 1u) used[out_reg] = 1;
        if (sph[19] & 2u) used[out_reg + 1] = 1;
    }
    int has_exit = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(insn[i]);
        if (!op || !shader_supported(insn[i], op->id) || !shader_stage_has(insn[i], op->id, stage)) {
            if (op && op->id == O_TEXS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TEXS encoding %u, fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 59, 1));
            else if (op && op->id == O_TLDS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TLDS encoding %u mask %u fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 50, 3), B(insn[i], 59, 1));
            else if (op && op->id == O_TXD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TXD dim %u offset %u", B(insn[i], 28, 3),
                         B(insn[i], 35, 1));
            else if (op && (op->id == O_HADD2_R || op->id == O_HADD2_C || op->id == O_HADD2_I || op->id == O_HADD2_32I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "HADD2 kind %d merge %u swa %u swb %u sat %u", op->id - O_HADD2_R, B(insn[i], 49, 2),
                         B(insn[i], 47, 2), B(insn[i], 28, 2), B(insn[i], 32, 1));
            else if (op && op->id >= O_XMAD_R && op->id <= O_XMAD_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "XMAD %d m%u c%u x%u s%u%u h%u p%u g%u",
                         op->id - O_XMAD_R, B(insn[i], 50, op->id == O_XMAD_CR || op->id == O_XMAD_RC ? 2 : 3),
                         B(insn[i], 47, 1),
                         op->id == O_XMAD_CR || op->id == O_XMAD_RC ? B(insn[i], 54, 1) : B(insn[i], 38, 1),
                         B(insn[i], 48, 1), B(insn[i], 49, 1), B(insn[i], 53, 1),
                         op->id == O_XMAD_RC ? 0 : B(insn[i], op->id == O_XMAD_CR ? 55 : 36, 1),
                         op->id == O_XMAD_RC ? 0 : B(insn[i], op->id == O_XMAD_CR ? 56 : 37, 1));
            else if (op && op->id == O_LDC)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "LDC size %u index %u addr %u",
                         B(insn[i], 48, 3), B(insn[i], 36, 5), B(insn[i], 8, 8));
            else if (op && op->id >= O_ISETP_R && op->id <= O_ISETP_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISETP kind %d cmp %u uns %u x %u cc %u",
                         op->id - O_ISETP_R, B(insn[i], 49, 3), B(insn[i], 48, 1), B(insn[i], 43, 1),
                         B(insn[i], 47, 1));
            else if (op && op->id >= O_F2F_R && op->id <= O_F2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2F kind %d dst %u src %u rnd %u int %u sat %u cc %u", op->id - O_F2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 42, 1), B(insn[i], 50, 1), B(insn[i], 47, 1));
            else if (op && op->id >= O_F2I_R && op->id <= O_F2I_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2I kind %d dst %u src %u rnd %u signed %u cc %u", op->id - O_F2I_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 12, 1), B(insn[i], 47, 1));
            else if (op && op->id >= O_I2F_R && op->id <= O_I2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2F kind %d dst %u src %u signed %u sel %u cc %u", op->id - O_I2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 13, 1), B(insn[i], 41, 2), B(insn[i], 47, 1));
            else if (op && op->id == O_ALD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ALD addr %u count %u index %u bit32 %u dst %u", B(insn[i], 20, 10), B(insn[i], 47, 2) + 1,
                         B(insn[i], 8, 8), B(insn[i], 32, 1), B(insn[i], 0, 8));
            else if (op && op->id == O_IPA)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IPA addr %u bit38 %u bit51 %u mode %u",
                         B(insn[i], 30, 8) * 4, B(insn[i], 38, 1), B(insn[i], 51, 1), B(insn[i], 54, 2));
            else if (op && op->id == O_IADD32I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD32I po %u x %u cc %u sat %u",
                         B(insn[i], 55, 2), B(insn[i], 53, 1), B(insn[i], 52, 1), B(insn[i], 54, 1));
            else if (op &&
                     (op->id == O_ISCADD_R || op->id == O_ISCADD_C || op->id == O_ISCADD_I || op->id == O_ISCADD32I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISCADD kind %d avg %u shift %u cc %u",
                         op->id - O_ISCADD_R, op->id == O_ISCADD32I ? 0 : B(insn[i], 48, 2),
                         B(insn[i], op->id == O_ISCADD32I ? 53 : 39, 5),
                         B(insn[i], op->id == O_ISCADD32I ? 52 : 47, 1));
            else if (op && (op->id == O_IADD_R || op->id == O_IADD_C || op->id == O_IADD_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD kind %d avg %u x %u cc %u sat %u",
                         op->id - O_IADD_R, B(insn[i], 48, 2), B(insn[i], 43, 1), B(insn[i], 47, 1), B(insn[i], 50, 1));
            else if (op && (op->id == O_I2I_R || op->id == O_I2I_C || op->id == O_I2I_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2I kind %d dst %u src %u signed %u%u sel %u neg %u abs %u", op->id - O_I2I_R,
                         B(insn[i], 8, 2), B(insn[i], 10, 2), B(insn[i], 12, 1), B(insn[i], 13, 1), B(insn[i], 41, 2),
                         B(insn[i], 45, 1), B(insn[i], 49, 1));
            else if (op && (op->id == O_SHR_R || op->id == O_SHR_C || op->id == O_SHR_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "SHR kind %d sign %u wrap %u reverse %u xmode %u cc %u", op->id - O_SHR_R, B(insn[i], 48, 1),
                         B(insn[i], 39, 1), B(insn[i], 40, 1), B(insn[i], 43, 2), B(insn[i], 47, 1));
            else if (op && (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ISET kind %d cmp %u uns %u x %u cc %u bool %u", op->id - O_ISET_R, B(insn[i], 49, 3),
                         B(insn[i], 48, 1), B(insn[i], 43, 1), B(insn[i], 47, 1), B(insn[i], 45, 2));
            else if (op && (op->id == O_TXQ || op->id == O_TXQ_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s q%u mask%u off%u", op->name,
                         B(insn[i], 22, 6), B(insn[i], 31, 4), B(insn[i], 36, 13));
            else if (op && (op->id == O_TLD || op->id == O_TLD_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s ty%u lod%u lc%u toff%u ms%u mask%u",
                         op->name, B(insn[i], 28, 3), B(insn[i], 55, 1), B(insn[i], 54, 1), B(insn[i], 35, 1),
                         B(insn[i], 50, 1), B(insn[i], 31, 4));
            else if (op && (op->id == O_TLD4 || op->id == O_TLD4_B || op->id == O_TLD4S))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "%s dc%u ao%u comp%u fp16%u da%u db%u mask%u off%u toff%u lc%u", op->name, B(insn[i], 50, 1),
                         B(insn[i],
                           op->id == O_TLD4S    ? 51
                           : op->id == O_TLD4_B ? 36
                                                : 54,
                           op->id == O_TLD4S ? 1 : 2),
                         B(insn[i],
                           op->id == O_TLD4S    ? 52
                           : op->id == O_TLD4_B ? 38
                                                : 56,
                           2),
                         op->id == O_TLD4S ? B(insn[i], 55, 1) : 0, B(insn[i], 0, 8), B(insn[i], 28, 8),
                         B(insn[i], 31, 4), B(insn[i], 36, 13),
                         B(insn[i],
                           op->id == O_TLD4_B  ? 36
                           : op->id == O_TLD4S ? 51
                                               : 54,
                           op->id == O_TLD4S ? 1 : 2),
                         op->id == O_TLD4_B ? B(insn[i], 40, 1) : 0);
            else if (op && op->id == O_AST)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "AST addr %u count %u idxreg %u reg %u",
                         B(insn[i], 20, 10), B(insn[i], 47, 2) + 1, B(insn[i], 8, 8), B(insn[i], 0, 8));
            else if (op && (op->id == O_TEX || op->id == O_TEX_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "%s ty%u lod%u dc%u mask%u off%u aoffi%u lc%u", op->name, B(insn[i], 28, 3),
                         B(insn[i], op->id == O_TEX_B ? 37 : 55, 3), B(insn[i], 50, 1), B(insn[i], 31, 4),
                         B(insn[i], 36, 13), B(insn[i], op->id == O_TEX_B ? 36 : 54, 1),
                         B(insn[i], op->id == O_TEX_B ? 40 : 58, 1));
            else /* the word too, to decode which form of it is missing */
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s %016llx",
                         op ? op->name : "unknown opcode", (unsigned long long)insn[i]);
            si->unsupported = 1;
            return NULL;
        }
        /* an opcode taken here that no family writes (op_family) would be dropped: refused, the no-ops aside */
        if (!op_family[op->id] && op->id != O_NOP && op->id != O_DEPBAR && op->id != O_VOTE_VTG) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s %016llx: no family writes it", op->name,
                     (unsigned long long)insn[i]);
            si->unsupported = 1;
            return NULL;
        }
        if (op->id == O_BRA || op->id == O_CAL || op->id == O_PRET) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "branch target");
            has_flow = 1;
            if (op->id != O_BRA) has_calls = 1;
        }
        if (op->id == O_RET) has_flow = has_calls = 1; /* a RET with no CAL pops an empty stack: it ends the program */
        if ((op->id == O_RED || op->id == O_ATOM) &&
            shader_global_atomic_opcode(insn[i], op->id == O_RED) == 0xffffffffu)
            needs_atomic_cas = 1;
        if (op->id == O_ATOMS && shader_shared_atomic_opcode(insn[i]) == 0xffffffffu) needs_atomic_cas = 1;
        if ((op->id == O_SURED && (B(insn[i], 24, 3) == 3 || B(insn[i], 24, 3) == 4)) ||
            (op->id == O_SUATOM && (B(insn[i], 29, 4) == 3 || B(insn[i], 29, 4) == 4)))
            needs_atomic_cas = 1;
        if (op->id == O_BRX) has_flow = 1;
        /* Thread and block ids require compute's invocation and workgroup builtins. */
        if (op->id == O_S2R && stage == ST_CS) {
            u32 sr = B(insn[i], 20, 8);
            needs_local_id |= sr >= 0x20 && sr <= 0x23;
            needs_group_id |= sr >= 0x25 && sr <= 0x27;
        }
        if (op->id == O_S2R && stage == ST_FS && B(insn[i], 20, 8) == 0x13) needs_demote = 1; /* the helper query */
        if (op->id == O_S2R || op->id == O_CS2R) {
            u32 sr = B(insn[i], 20, 8);
            int scope_bit = sr >= 0x50 && sr <= 0x53 ? (sr < 0x52 ? 1 : 2) : 0;
            if (scope_bit && !(g_shader_clock_scopes & scope_bit)) return direct_reject(si, "shader clock unavailable");
            if (scope_bit) needs_clock = 1;
        }
        if (op->id == O_PIXLD) {
            if (B(insn[i], 31, 3) == 5)
                needs_sample_id = 1;
            else
                needs_coverage = 1;
        }
        if (op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "control stack target");
            has_flow = has_stack = 1;
        }
        if (op->id == O_PEXIT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "PEXIT target");
            has_flow = has_stack = has_pexit = 1;
        }
        if (op->id == O_EXIT && !flow_always(insn[i])) has_conditional_exit = 1;
        if (op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT) has_flow = has_stack = 1;
        if (((op->id >= O_FADD_R && op->id <= O_FFMA_I) || op->id == O_FFMA32I) && op->id != O_FMNMX_R &&
            op->id != O_FMNMX_C && op->id != O_FMNMX_I) {
            u32 mode = op->id == O_FADD32I                        ? B(insn[i], 55, 1)
                       : op->id >= O_FADD_R && op->id <= O_FADD_I ? B(insn[i], 44, 1)
                       : op->id >= O_FMUL_R && op->id <= O_FMUL_I ? B(insn[i], 44, 2)
                                                                  : B(insn[i], 53, 2);
            if (mode & 1)
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        if (op->id == O_FSWZADD) {
            if (B(insn[i], 44, 1))
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        u32 d = B(insn[i], 0, 8), a = B(insn[i], 8, 8), b = B(insn[i], 20, 8);
        u32 guard = B(insn[i], 16, 3);
        if (guard < 7) pred_used[guard] = 1;
        if (d < 255 && op->id != O_EXIT && op->id != O_KIL && op->id != O_BRA && op->id != O_BRX && op->id != O_SSY &&
            op->id != O_PBK && op->id != O_PCNT && op->id != O_SYNC && op->id != O_BRK && op->id != O_CONT &&
            op->id != O_AST && op->id != O_NOP && op->id != O_VOTE_VTG && op->id != O_DEPBAR && op->id != O_CAL &&
            op->id != O_PRET && op->id != O_RET && op->id != O_FSETP_R && op->id != O_FSETP_C && op->id != O_FSETP_I &&
            op->id != O_HSETP2_R && op->id != O_HSETP2_C && op->id != O_HSETP2_I && op->id != O_DSETP_R &&
            op->id != O_DSETP_C && op->id != O_DSETP_I && op->id != O_ISETP_R && op->id != O_ISETP_C &&
            op->id != O_ISETP_I && op->id != O_VSETP && op->id != O_PSETP && op->id != O_CSETP && op->id != O_R2P_R &&
            op->id != O_R2P_C && op->id != O_R2P_I && op->id != O_FCHK_R && op->id != O_FCHK_C && op->id != O_FCHK_I &&
            op->id != O_MEMBAR && op->id != O_CCTL && op->id != O_CCTLL && op->id != O_IDE && op->id != O_BPT &&
            op->id != O_PEXIT)
            used[d] = 1;
        if (op->id == O_AL2P && !shader_al2p_predicate_unseen(insn, n, i))
            return direct_reject(si, "AL2P range predicate read");
        if (op->id == O_ALD && a != 255) {
            /* an indexed read can reach any generic input the header's map lists (bytes 0x18 to 0x27, a bit for
               each of an input's four components) */
            u32 width = B(insn[i], 47, 2) + 1, vertex = B(insn[i], 39, 8);
            used[a] = 1;
            if (between && vertex < 255) used[vertex] = 1;
            for (u32 k = 0; k < width; k++)
                if (d + k < 255) used[d + k] = 1;
            for (u32 v = 0; v < 32; v++)
                if ((sph[6 + v / 8] >> (v % 8 * 4)) & 15) inputs_used[v] = 1;
        } else if (op->id == O_ALD || op->id == O_AST) {
            u32 addr = B(insn[i], 20, 10), width = B(insn[i], 47, 2) + 1, vertex = B(insn[i], 39, 8);
            int input = op->id == O_ALD && !(stage == ST_TCS && B(insn[i], 32, 1));
            if (op->id == O_AST && a != 255) indexed_store = 1;
            if (between && vertex < 255) used[vertex] = 1;
            if (stage == ST_TCS && op->id == O_AST) invocation_used = 1; /* the invocation's output vertex */
            for (u32 k = 0; k < width; k++) {
                if (d + k < 255) used[d + k] = 1;
                u32 location = addr + k * 4;
                if (B(insn[i], 31, 1)) { /* the levels' 6 words and 2 of padding, then the patch's vectors */
                    if (location >= 0x20)
                        patch_used[(location - 0x20) >> 4] = 1;
                    else if (location < 0x18)
                        levels_used |= location < 0x10 ? 1 : 2;
                } else if (input && location >= 0x2f8 && location <= 0x2fc)
                    needs_instance = 1;
                else if (input && (location == 0x2f0 || location == 0x2f4))
                    tess_coord_used = 1;
                else if (input && location == 0x60)
                    primitive_id_used = 1;
                else if (input && location < 0x80)
                    position_in_used = 1;
                else if (input)
                    inputs_used[(location - 0x80) >> 4] = 1;
                else if (location == 0x64)
                    layer_output_used = 1;
                else if (location == 0x68)
                    viewport_used = 1;
                else if (location == 0x6c)
                    point_size_used = 1;
                else if (location >= 0x2c0 && location < 0x2e0) {
                    u32 bit = 1u << ((location - 0x2c0) / 4);
                    if (si->clip_enable & bit) clip_used |= bit;
                } else if (location == 0x3a0) {
                    /* the viewport mask, RZ or a register holding a constant (one game's culling program writes 0 or a
                       MOV32I's 1 and emits no vertex): viewport 0 alone or none, no broadcast, which the primitive's
                       own viewport (0 where the program writes no index) already is */
                    u32 mask = 0;
                    if (d != 255 && !shader_direct_track_const(insn, n, i, d, &mask, 0)) {
                        si->unsupported = 1;
                        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                                 "viewport mask R%u at %x: no constant", d, i * 8);
                        return NULL;
                    }
                    if (mask > 1) {
                        si->unsupported = 1;
                        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                                 "viewport mask %#x at %x: a broadcast", mask, i * 8);
                        return NULL;
                    }
                    viewport_mask_one |= mask;
                    continue;
                } else if (location >= 0x80)
                    outputs_used[(location - 0x80) >> 4] = 1;
                else
                    position_used = 1;
            }
        }
        if (op->id == O_ISBERD) used[a] = 1;
        if (op->id == O_S2R && B(insn[i], 20, 8) == 0x11) invocation_used = 1;
        if (op->id == O_IMUL_R || op->id == O_IMAD_R || op->id == O_IMAD_RC) {
            if (b < 255 && op->id != O_IMAD_RC) used[b] = 1;
            if (op->id == O_IMAD_RC && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_IMAD_R || op->id == O_IMAD_CR || op->id == O_IMAD_I)
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_IMAD32I && d < 255) used[d] = 1;
        if ((op->id >= O_IMUL_R && op->id <= O_IMAD32I) && a < 255) used[a] = 1;
        if (op->id == O_MOV_R || op->id == O_LOP3_R || op->id == O_IMNMX_R || op->id == O_XMAD_R || op->id == O_BFE_R ||
            op->id == O_ICMP_R || op->id == O_IADD3_R || op->id == O_FADD_R || op->id == O_FMUL_R ||
            op->id == O_FFMA_R || op->id == O_FMNMX_R || op->id == O_FSET_R || op->id == O_RRO_R || op->id == O_FLO_R ||
            op->id == O_POPC_R) {
            if (b < 255) used[b] = 1;
        }
        if ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I || op->id == O_FADD32I ||
             op->id == O_FFMA32I || op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I ||
             op->id == O_FMUL32I || op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR ||
             op->id == O_FFMA_I || op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
             op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I || op->id == O_MUFU) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_AL2P || op->id == O_BRX || (op->id >= O_P2R_R && op->id <= O_R2P_I) || op->id == O_LOP3_R ||
             op->id == O_LOP3_C || op->id == O_LOP3_I || op->id == O_IMNMX_R || op->id == O_IMNMX_C ||
             op->id == O_IMNMX_I || op->id == O_XMAD_CR || op->id == O_XMAD_RC || op->id == O_XMAD_R ||
             op->id == O_IADD3_R || op->id == O_IADD3_C || op->id == O_IADD3_I || op->id == O_ICMP_R ||
             op->id == O_ICMP_RC || op->id == O_ICMP_CR || op->id == O_ICMP_I || op->id == O_FCMP_R ||
             op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_FCMP_I || op->id == O_BFI_R ||
             op->id == O_BFI_RC || op->id == O_BFI_CR || op->id == O_BFI_I || op->id == O_IADD32I ||
             op->id == O_LOP32I || op->id == O_XMAD_I || op->id == O_IADD_R || op->id == O_IADD_C ||
             op->id == O_IADD_I || op->id == O_LDG || op->id == O_STG || op->id == O_RED || op->id == O_ATOM ||
             op->id == O_ATOM_CAS || op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I || op->id == O_SEL_R ||
             op->id == O_SEL_C || op->id == O_SEL_I || op->id == O_ISCADD_R || op->id == O_ISCADD_C ||
             op->id == O_ISCADD_I || op->id == O_ISCADD32I || (op->id >= O_BFE_R && op->id <= O_BFE_I) ||
             op->id == O_SHL_R || op->id == O_SHL_C || op->id == O_SHL_I || op->id == O_SHR_R || op->id == O_SHR_C ||
             op->id == O_SHR_I || op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_ISCADD_R || op->id == O_IADD_R || op->id == O_LOP_R || op->id == O_SEL_R ||
             op->id == O_P2R_R || op->id == O_R2P_R) &&
            b < 255)
            used[b] = 1;
        if (op->id == O_SEL_R || op->id == O_SEL_C || op->id == O_SEL_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if ((op->id == O_XMAD_I || op->id == O_XMAD_R || op->id == O_XMAD_RC || op->id == O_XMAD_CR) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_F2F_R || op->id == O_F2I_R || op->id == O_I2F_R) && b < 255) used[b] = 1;
        /* the conversions' 64-bit sides (format 3) are register pairs: an F64 (F2F, F2I's source, I2F's result) or
           a 64-bit integer (F2I's result, I2F's source) */
        if ((op->id >= O_F2F_R && op->id <= O_F2F_I) || (op->id >= O_F2I_R && op->id <= O_F2I_I) ||
            (op->id >= O_I2F_R && op->id <= O_I2F_I)) {
            int f2f = op->id <= O_F2F_I && op->id >= O_F2F_R, f2i = op->id >= O_F2I_R && op->id <= O_F2I_I;
            u32 dst_format = B(insn[i], 8, 2), src_format = B(insn[i], 10, 2);
            if ((src_format == 3 && (f2f || f2i)) || (dst_format == 3 && !f2i)) needs_f64 = 1;
            if ((dst_format == 3 && f2i) || (src_format == 3 && !f2f && !f2i)) needs_i64 = 1;
            if (dst_format == 3 && d < 254) used[d + 1] = 1;
            if (src_format == 3 && (op->id == O_F2F_R || op->id == O_F2I_R || op->id == O_I2F_R) && b < 254)
                used[b + 1] = 1;
        }
        if (op->id == O_I2I_R && b < 255) used[b] = 1;
        if ((op->id == O_ICMP_R || op->id == O_ICMP_RC || op->id == O_ICMP_CR || op->id == O_ICMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_FCMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC) && b < 255) used[b] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC || op->id == O_BFI_CR || op->id == O_BFI_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC) && b < 255) used[b] = 1;
        if ((op->id == O_IADD3_R || op->id == O_IADD3_C || op->id == O_IADD3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_LOP3_R || op->id == O_LOP3_C || op->id == O_LOP3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_LDC) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 48, 3) == 5 && d + 1 < 255) used[d + 1] = 1;
            u32 bank = B(insn[i], 36, 5);
            cbuf_used[bank] = 1;
            cbuf_max[bank] = 65536;
        }
        if (op->id == O_LDG || op->id == O_STG) {
            u32 count = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + count > 256) return direct_reject(si, "global memory register range");
            for (u32 k = 0; k < count; k++)
                if (d + k < 255) used[d + k] = 1;
            /* a 64-bit address's high word, which an access whose buffer is found when it runs compares */
            if (B(insn[i], 45, 1) && a + 1 < 255) used[a + 1] = 1;
        }
        if (op->id == O_RED && d < 255) used[d] = 1;
        if (op->id == O_ATOM && b < 255) used[b] = 1;
        if (op->id == O_VADD || op->id == O_VABSDIFF || op->id == O_VSET || op->id == O_VSHL || op->id == O_VSHR) {
            /* Signed intermediate, saturation and secondary operations use 64 bits even for narrow source lanes.
               vk_translate_spirv reports shaderInt64 unavailable if the device cannot run these shaders. */
            needs_i64 = 1;
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id >= O_PRMT_R && op->id <= O_PRMT_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_PRMT_R && b < 255) used[b] = 1;
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1; /* C, or the RC form's selector */
        }
        if (op->id == O_ATOM_CAS || op->id == O_ATOMS_CAS) {
            if (op->id == O_ATOMS_CAS) needs_shared = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (b < 254) used[b + 1] = 1;
        }
        if (op->id == O_LDL || op->id == O_STL) {
            needs_local = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "local memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_LDS || op->id == O_STS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "shared memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_ATOMS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (shader_shared_atomic_opcode(insn[i]) == 0xfffffffeu) {
                if (stage != ST_CS || (u64)si->cs_local[0] * si->cs_local[1] * si->cs_local[2] != 1)
                    return direct_reject(si, "64-bit shared exchange requires one invocation");
                if (b + 1 < 255) used[b + 1] = 1;
                if (d + 1 < 255) used[d + 1] = 1;
            }
        }
        if (op->id == O_BAR) has_barrier = 1;
        if (op->id == O_ISETP_R || op->id == O_ISETP_C || op->id == O_ISETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_ISETP_R && b < 255) used[b] = 1;
            u32 pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3), pc = B(insn[i], 39, 3);
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            if (pc < 7) pred_used[pc] = 1;
        }
        if (op->id == O_PSETP) {
            u32 pp[] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 5; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_PSET) {
            u32 pred_ids[3] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pred_ids[k] < 7) pred_used[pred_ids[k]] = 1;
        }
        if (op->id == O_CSETP || op->id == O_CSET) {
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_VOTE) {
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 45, 3) < 7) pred_used[B(insn[i], 45, 3)] = 1;
        }
        if (op->id == O_SHFL) {
            if (a < 255) used[a] = 1;
            if (!B(insn[i], 28, 1) && b < 255) used[b] = 1;
            if (!B(insn[i], 29, 1) && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
            if (B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        }
        if (op->id == O_IMNMX_R || op->id == O_IMNMX_C || op->id == O_IMNMX_I)
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        if (op->id >= O_P2R_R && op->id <= O_R2P_I && !B(insn[i], 40, 1))
            for (u32 p = 0; p < 7; p++) pred_used[p] = 1;
        if ((op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I) && B(insn[i], 48, 3) < 7)
            pred_used[B(insn[i], 48, 3)] = 1;
        if (op->id == O_LOP3_R && B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        if (shader_sparse_predicate(insn[i], op->id) < 7) pred_used[shader_sparse_predicate(insn[i], op->id)] = 1;
        if (op->id == O_SHL_R && b < 255) used[b] = 1;
        if (op->id == O_SHF_LR || op->id == O_SHF_LI || op->id == O_SHF_RR || op->id == O_SHF_RI) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_SHF_LR || op->id == O_SHF_RR) && b < 255) used[b] = 1;
            u32 hi = B(insn[i], 39, 8);
            if (hi < 255) used[hi] = 1;
        }
        if (op->id == O_LEA_LR || op->id == O_LEA_LC || op->id == O_LEA_LI || op->id == O_LEA_HR ||
            op->id == O_LEA_HC) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_LEA_LR || op->id == O_LEA_HR) && b < 255) used[b] = 1;
            if (op->id == O_LEA_HR || op->id == O_LEA_HC) {
                u32 hi = B(insn[i], 39, 8);
                if (hi < 255) used[hi] = 1;
            } else if (B(insn[i], 48, 3) < 7)
                pred_used[B(insn[i], 48, 3)] = 1;
        }
        if ((op->id == O_SHR_R || op->id == O_ISET_R) && b < 255) used[b] = 1;
        if (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) {
            u32 pc = B(insn[i], 39, 3);
            if (pc < 7) pred_used[pc] = 1;
        }
        if ((op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_MUFU && (B(insn[i], 20, 4) == 6 || B(insn[i], 20, 4) == 7)) needs_f64 = 1;
        if (op->id >= O_FCHK_R && op->id <= O_FCHK_I && B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
        if (op->id == O_CS2R && d < 254) used[d] = used[d + 1] = 1;
        if (op->id == O_MUFU || op->id == O_RRO_R || op->id == O_RRO_C || op->id == O_RRO_I || op->id == O_FFMA32I ||
            op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I ||
            op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
            ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I) &&
             (B(insn[i], 46, 1) || B(insn[i], 49, 1) || B(insn[i], 50, 1))) ||
            (op->id == O_FADD32I && (B(insn[i], 54, 1) || B(insn[i], 57, 1))) ||
            ((op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I) && B(insn[i], 50, 1)) ||
            (op->id == O_FMUL32I && B(insn[i], 55, 1)) || (op->id >= O_F2F_R && op->id <= O_F2F_I) ||
            (op->id >= O_F2I_R && op->id <= O_F2I_I) || (op->id >= O_I2F_R && op->id <= O_I2F_I))
            needs_glsl = 1;
        if (op->id == O_IMUL_C || op->id == O_IMAD_RC || op->id == O_IMAD_CR) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
            if (stage == ST_VS && idx == 0 && (off == SH_CB0_BASE_VERTEX || off == SH_CB0_BASE_INSTANCE))
                needs_draw_params = 1;
        }
        if (op->id == O_LOP3_C || op->id == O_IMNMX_C || op->id == O_XMAD_CR || op->id == O_XMAD_RC ||
            op->id == O_BFE_C || op->id == O_IADD3_C || op->id == O_LEA_LC || op->id == O_LEA_HC || op->id == O_F2I_C ||
            op->id == O_I2F_C || op->id == O_I2I_C || op->id == O_F2F_C || op->id == O_FLO_C || op->id == O_POPC_C ||
            op->id == O_P2R_C || op->id == O_R2P_C || op->id == O_ICMP_RC || op->id == O_ICMP_CR ||
            op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_BFI_RC || op->id == O_BFI_CR ||
            op->id == O_MOV_C || op->id == O_IADD_C || op->id == O_ISCADD_C || op->id == O_LOP_C || op->id == O_SEL_C ||
            op->id == O_HADD2_C || op->id == O_HMUL2_C || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR ||
            op->id == O_ISETP_C || op->id == O_ISET_C || op->id == O_SHL_C || op->id == O_SHR_C || op->id == O_FADD_C ||
            op->id == O_FMUL_C || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FMNMX_C ||
            op->id == O_RRO_C || op->id == O_FSETP_C || op->id == O_FSET_C || op->id == O_HSET2_C ||
            op->id == O_HSETP2_C || op->id == O_DADD_C || op->id == O_DMUL_C || op->id == O_DFMA_RC ||
            op->id == O_DFMA_CR || op->id == O_DMNMX_C || op->id == O_DSET_C || op->id == O_DSETP_C ||
            op->id == O_OUT_C || op->id == O_PRMT_RC || op->id == O_PRMT_CR) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
            if (stage == ST_VS && idx == 0 && (off == SH_CB0_BASE_VERTEX || off == SH_CB0_BASE_INSTANCE))
                needs_draw_params = 1;
        }
        if (op->id == O_OUT_R && b < 255) used[b] = 1;
        if (op->id == O_FSETP_R || op->id == O_FSETP_C || op->id == O_FSETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_FSETP_R && b < 255) used[b] = 1;
            u32 pc = B(insn[i], 39, 3), pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3);
            if (pc < 7) pred_used[pc] = 1;
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            needs_glsl = 1;
        }
        if (op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if (op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
            if (B(insn[i], 54, 1) || B(insn[i], 44, 1)) needs_glsl = 1;
        }
        if (op->id == O_TEXS) {
            u32 off = B(insn[i], 36, 13);
            u32 encoding = B(insn[i], 53, 4);
            int shadow = shader_texs_shadow(encoding), type = shader_texs_type(encoding),
                array = shader_texs_array(encoding);
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].shadow == shadow && textures[slot].type == type &&
                    textures[slot].array == array && !textures[slot].bindless && !textures[slot].flags)
                    break;
            if (slot == texture_count) {
                if (texture_count >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].shadow = (u8)shadow;
                textures[slot].array = (u8)array;
                texture_count++;
            }
            tex_read |= 1ull << slot;
            tex_elsewhere |= 1ull << slot;
            if (!B(insn[i], 59, 1)) needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (encoding != 0 && shader_texs_y(encoding, a, b) == a + 1 && a + 1 < 255) used[a + 1] = 1;
            if (array && a + 1 < 255) used[a + 1] = 1;
            if (shader_texs_level(encoding, b) < 255) used[shader_texs_level(encoding, b)] = 1;
            if (shadow && shader_texs_dref(encoding, b) < 255) used[shader_texs_dref(encoding, b)] = 1;
            shader_texs_mark_outputs(insn[i], used);
        }
        /* fragment fetches and size queries may read a scaled target (shader_scaled_coords) */
        if ((op->id == O_TLD || op->id == O_TLD_B || op->id == O_TLDS || op->id == O_TXQ || op->id == O_TXQ_B) &&
            stage == ST_FS)
            needs_texel_scale = needs_glsl = 1;
        if (op->id == O_TLD || op->id == O_TLD_B) {
            u32 indexed = 0, ldc = 0;
            u32 off = B(insn[i], 36, 13), ty = B(insn[i], 28, 3);
            u32 bank = 0, bank2 = 0, off2 = 0;
            int bindless = op->id == O_TLD_B;
            int handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, b);
            int type = ty >> 1, array = ty & 1, multisample = (int)B(insn[i], 50, 1);
            int read_slot =
                shader_direct_add_typed_slot(textures, &texture_count, off, type, array, 0, multisample ? 2 : 0,
                                             bindless, bank, handle_count, bank2, off2, indexed, ldc);
            if (read_slot < 0) return direct_reject(si, "texture descriptor count");
            tex_read |= 1ull << read_slot;
            if (op->id == O_TLD ? !shader_direct_own_pixel_fetch(insn, n, i) : 1) tex_elsewhere |= 1ull << read_slot;
            u32 coordinate_count = (type == 0 ? 1u : type == 1 ? 2u : 3u) + (u32)array;
            for (u32 k = 0; k < coordinate_count; k++)
                if (a + k < 255) used[a + k] = 1;
            if (bindless && b < 255) used[b] = 1;
            u32 meta = b + bindless;
            if (B(insn[i], 55, 1) && meta < 255) used[meta++] = 1;
            if (B(insn[i], 35, 1) && meta < 255) used[meta++] = 1;
            if (multisample && meta < 255) used[meta] = 1;
            u32 target = d, mask = B(insn[i], 31, 4);
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_SULD || op->id == O_SUST || op->id == O_SURED || op->id == O_SUATOM) {
            int dim = (int)B(insn[i], 33, 3), buffer = dim == 1;
            u32 bank, off, handle;
            int bindless;
            /* a fragment program's 2D loads are texel fetches, as a compute program's are: render.c binds the raw
               view their slot asks for. A vertex program stores and reduces through images as a fragment one does
               (one game filters a texture so, a vertex for each texel, with no compute at all). Buffer surfaces
               too: a base pass writes its texture feedback into one */
            if (stage != ST_CS && stage != ST_FS && (stage != ST_VS || op->id == O_SULD))
                return direct_reject(si, "surface operation outside supported stage");
            bank = 0;
            off = B(insn[i], 36, 13);
            handle = B(insn[i], 39, 8);
            bindless = op->id == O_SUATOM ? (int)B(insn[i], 54, 1) : !B(insn[i], 51, 1);
            if (bindless && !shader_direct_track_image_handle(insn, n, i, handle, &bank, &off, 0))
                return shader_untracked_handle(si, "surface", insn, i, handle);
            if (a < 255) used[a] = 1;
            if (!buffer && a + 1 < 255) used[a + 1] = 1;
            if ((dim == 4 || dim == 5) && a + 2 < 255) used[a + 2] = 1;
            if (bindless && handle < 255) used[handle] = 1;
            int raw = op->id == O_SULD && B(insn[i], 52, 1);
            u32 mask = raw ? 1u : B(insn[i], 20, 4), target = d;
            if (op->id == O_SURED) {
                if (target < 255) used[target] = 1;
            } else if (op->id == O_SUATOM) {
                if (d < 255) used[d] = 1;
                if (b < 255) used[b] = 1;
            } else
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        if (target < 255) used[target] = 1;
                        target++;
                    }
            /* buffer loads read the storage texel buffer: a sampled 1D image of a buffer is one row of at most
               16384 texels, and the dispatch's own stores and atomics wouldn't reach it */
            if (op->id != O_SULD || buffer) {
                /* raw loads and atomics see R32UI; formatted loads and stores see the header's format */
                int r32ui = op->id != O_SUST && (op->id != O_SULD || raw);
                int type = dim; /* 1, 3, 4 or 5: the forms shader_supported() takes here */
                int slot = shader_direct_image_slot(&image_info, off, bindless, bank, r32ui, type);
                if (slot == image_info.nimg) {
                    if (slot >= 8) return direct_reject(si, "storage image descriptor count");
                    int different_format = !!(si->rt_int_mask & (1u << (8 + slot)));
                    int kind = r32ui ? 1 : (bindless != different_format ? 2 : 0);
                    if (!r32ui && (si->rt_sint_mask & (1u << (8 + slot)))) kind = 3;
                    image_info.img[slot].cbuf_off = off;
                    image_info.img[slot].type = (u8)type;
                    image_info.img[slot].bindless = (u8)bindless;
                    image_info.img[slot].bl_cbuf = (u8)bank;
                    image_info.img[slot].kind = (u8)kind;
                    image_info.nimg++;
                }
                if (bindless) {
                    cbuf_used[bank] = 1;
                    if (off * 4 + 16 > cbuf_max[bank]) cbuf_max[bank] = ((off * 4) | 15u) + 1;
                }
            } else {
                int slot = 0;
                int sample_type = dim == 5 ? 2 : 1, arrayed = dim == 4;
                while (slot < texture_count &&
                       (textures[slot].cbuf_off != off || textures[slot].type != sample_type ||
                        textures[slot].array != arrayed || textures[slot].shadow ||
                        textures[slot].bindless != bindless || (textures[slot].flags & 3) != (raw ? 1 : 0) ||
                        (bindless && textures[slot].bl_cbuf != bank)))
                    slot++;
                if (slot == texture_count) {
                    if (slot >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                    textures[slot].cbuf_off = off;
                    textures[slot].type = (u8)sample_type;
                    textures[slot].array = (u8)arrayed;
                    textures[slot].bindless = (u8)bindless;
                    textures[slot].flags = raw ? 1 : 0;
                    textures[slot].bl_cbuf = (u8)bank;
                    texture_count++;
                }
                tex_read |= 1ull << slot;
                tex_elsewhere |= 1ull << slot;
            }
        }
        if (shader_half_arithmetic(op->id)) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if ((op->id == O_HADD2_R || op->id == O_HMUL2_R || op->id == O_HFMA2_R) && b < 255) used[b] = 1;
            if ((op->id == O_HFMA2_R || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR || op->id == O_HFMA2_I) &&
                B(insn[i], 39, 8) < 255)
                used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_HSET2_R || op->id == O_HSET2_C || op->id == O_HSET2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSET2_R && b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        }
        if (op->id == O_HSETP2_R || op->id == O_HSETP2_C || op->id == O_HSETP2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSETP2_R && b < 255) used[b] = 1;
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
            (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) ||
            (op->id >= O_DSET_R && op->id <= O_DSET_I) || (op->id >= O_DSETP_R && op->id <= O_DSETP_I)) {
            int right_reg;
            needs_f64 = needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (a < 254) used[a + 1] = 1;
            right_reg = op->id == O_DADD_R || op->id == O_DMUL_R || op->id == O_DFMA_R || op->id == O_DFMA_RC ||
                        op->id == O_DMNMX_R || op->id == O_DSET_R || op->id == O_DSETP_R;
            if (right_reg && b < 255) used[b] = 1;
            if (right_reg && b < 254) used[b + 1] = 1;
            if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
                (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I)) {
                if (d < 254) used[d + 1] = 1;
                if ((op->id >= O_DFMA_R && op->id <= O_DFMA_I) && B(insn[i], 39, 8) < 255) {
                    used[B(insn[i], 39, 8)] = 1;
                    if (B(insn[i], 39, 8) < 254) used[B(insn[i], 39, 8) + 1] = 1;
                }
            }
            if ((op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) || (op->id >= O_DSET_R && op->id <= O_DSET_I) ||
                (op->id >= O_DSETP_R && op->id <= O_DSETP_I))
                if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (op->id >= O_DSETP_R && op->id <= O_DSETP_I) {
                if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
                if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
            }
        }
        if (op->id == O_FSWZADD) {
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
        }
        if (op->id == O_VMNMX) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VMAD) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VSETP) {
            needs_i64 = 1;
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
            if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
        }
        if (op->id == O_TLDS) {
            u32 off = B(insn[i], 36, 13);
            u32 enc = B(insn[i], 53, 4);
            /* encoding 6 fetches a sample (Rb) of a multisample texture */
            int array = enc == 8, type = enc == 0 || enc == 1 ? 0 : enc == 7 ? 2 : 1, multisample = enc == 6;
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].type == type && textures[slot].array == array &&
                    !textures[slot].shadow && !textures[slot].bindless && textures[slot].flags == (multisample ? 2 : 0))
                    break;
            if (slot == texture_count) {
                if (texture_count >= SH_MAXTEX) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                textures[slot].flags = (u8)(multisample ? 2 : 0);
                texture_count++;
            }
            tex_read |= 1ull << slot;
            if (!shader_direct_own_pixel_fetch(insn, n, i)) tex_elsewhere |= 1ull << slot;
            if (!B(insn[i], 59, 1)) needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if ((enc == 4 || enc == 5 || enc == 6 || enc == 7 || enc == 12) && a + 1 < 255) used[a + 1] = 1;
            if (b < 255) used[b] = 1;
            if ((enc == 12 || array) && b + 1 < 255) used[b + 1] = 1;
            if (!B(insn[i], 59, 1)) {
                u32 regs[4], comps[4];
                int outputs = shader_texs_outputs(insn[i], regs, comps);
                needs_f16 = 1;
                if (d < 255) used[d] = 1;
                if (outputs > 2 && B(insn[i], 28, 8) < 255) used[B(insn[i], 28, 8)] = 1;
            } else {
                shader_texs_mark_outputs(insn[i], used);
            }
        }
        if (op->id == O_TEX || op->id == O_TEX_B) {
            int bindless = op->id == O_TEX_B;
            u32 mode = bindless ? B(insn[i], 37, 3) : B(insn[i], 55, 3);
            u32 offset_enabled = B(insn[i], bindless ? 36 : 54, 1);
            u32 source = b + bindless;
            u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
            u32 offset_source = source + has_level;
            u32 off, bank, bank2, off2, indexed = 0, ldc = 0;
            int handle_count;
            if (offset_enabled) {
                u32 packed;
                int known = shader_direct_track_const(insn, n, i, offset_source, &packed, 0);
                if (!known) needs_image_query = 1; /* the offset moves the coordinates by the texture's size */
            }
            off = B(insn[i], 36, 13);
            bank = 0;
            bank2 = 0;
            off2 = 0;
            handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, b);
            int type = B(insn[i], 28, 3) >> 1;
            int array = B(insn[i], 28, 1), shadow = B(insn[i], 50, 1);
            int read_slot = shader_direct_add_typed_slot(textures, &texture_count, off, type, array, shadow, 0,
                                                         bindless, bank, handle_count, bank2, off2, indexed, ldc);
            if (read_slot < 0) return direct_reject(si, "texture descriptor count");
            tex_read |= 1ull << read_slot;
            tex_elsewhere |= 1ull << read_slot;
            if (a < 255) used[a] = 1;
            if ((type != 0 || array) && a + 1 < 255) used[a + 1] = 1;
            if ((type >= 2 || array) && a + 2 < 255) used[a + 2] = 1;
            if (type == 3 && array && a + 3 < 255) used[a + 3] = 1;
            if (b < 255) used[b] = 1;
            if (has_level && source < 255) used[source] = 1;
            if (offset_enabled && offset_source < 255) used[offset_source] = 1;
            if (shadow && offset_source + offset_enabled < 255) used[offset_source + offset_enabled] = 1;
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    if (dst < 255) used[dst] = 1;
                    if (dst < 255) dst++;
                }
        }
        /* Ryujinx InstEmitTexture.EmitTmml: TMML.B takes its handle in Rb; Ra holds an array's layer, then coordinates. */
        if (op->id == O_TMML || op->id == O_TMML_B) {
            int bindless = op->id == O_TMML_B;
            u32 ty = B(insn[i], 28, 3), type = ty >> 1, array = ty & 1;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, b);
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (shader_direct_add_typed_slot(textures, &texture_count, off, type, array, 0, 0, bindless, bank,
                                             handle_count, bank2, off2, indexed, ldc) < 0)
                return direct_reject(si, "texture descriptor count");
            for (u32 k = 0; k < array + (type == 0 ? 1 : type == 1 ? 2 : 3); k++)
                if (a + k < 255) used[a + k] = 1;
            if (bindless && b < 255) used[b] = 1;
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component) && dst < 255) used[dst++] = 1;
            needs_image_query = 1;
        }
        /* TXD and TXD.B: in Ra TXD.B's handle, the coordinates, then the array layer (bits 0-15) and the offsets (from
           bit 16) in one register; in Rb each coordinate's X derivative then its Y one. Mesa's NAK packs them so for
           Maxwell, and Eden reads them so (Ryujinx takes every X derivative first). */
        if (op->id == O_TXD || op->id == O_TXD_B) {
            int bindless = op->id == O_TXD_B;
            u32 ty = B(insn[i], 28, 3), type = ty >> 1, array = ty & 1, count = type == 0 ? 1 : type == 1 ? 2 : 3;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, a);
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, a, stage, &ldc);
            int read_slot = shader_direct_add_typed_slot(textures, &texture_count, off, type, array, 0, 0, bindless,
                                                         bank, handle_count, bank2, off2, indexed, ldc);
            if (read_slot < 0) return direct_reject(si, "texture descriptor count");
            tex_read |= 1ull << read_slot;
            tex_elsewhere |= 1ull << read_slot;
            u32 coord = a + (u32)bindless, aoffi = B(insn[i], 35, 1);
            for (u32 k = 0; k < count + (array || aoffi); k++)
                if (a + k + (u32)bindless < 255) used[coord + k] = 1;
            if (bindless && a < 255) used[a] = 1;
            for (u32 k = 0; k < 2 * count; k++)
                if (b + k < 255) used[b + k] = 1;
            if (aoffi) {
                u32 packed;
                if (!shader_direct_track_const(insn, n, i, coord + count, &packed, 0))
                    needs_image_query = 1; /* the offset moves the coordinates by the texture's size */
            }
            u32 mask = B(insn[i], 31, 4), dst = d;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component) && dst < 255) used[dst++] = 1;
        }
        if (op->id == O_TXQ || op->id == O_TXQ_B) {
            /* TXQ.B takes the handle in Ra and the level in Ra + 1 */
            int bindless = op->id == O_TXQ_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, a);
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, a, stage, &ldc);
            u32 qtype = 1, qarray = 0;
            int guessed = 0, qslot;
            if (!indexed && !shader_direct_query_type(insn, n, bindless, bank, off, &qtype, &qarray)) {
                /* nothing else in the program reads that texture (one game queries a volume through a bound handle
                   and fetches it through a bindless one, which no tracking joins): the header's dimension, from the
                   key, once the renderer has bound it; 2D before that */
                u8 hint = texture_count < SH_MAXTEX ? si->tex_query_hint[texture_count] : 0;
                if (hint) {
                    qtype = (hint - 1) & 3;
                    qarray = (hint - 1) >> 2 & 1;
                }
                guessed = 1;
            }
            qslot = shader_direct_add_typed_slot(textures, &texture_count, off, qtype, qarray, 0, 0, bindless, bank,
                                                 handle_count, bank2, off2, indexed, ldc);
            if (qslot < 0) return direct_reject(si, "texture descriptor count");
            if (guessed) tex_query |= 1ull << qslot;
            needs_image_query = 1;
            if (a < 255) used[a] = 1;
            if (bindless && a + 1 < 255) used[a + 1] = 1;
            u32 target = d;
            for (u32 k = 0; k < 4; k++)
                if (B(insn[i], 31 + k, 1)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_TLD4S || op->id == O_TLD4 || op->id == O_TLD4_B) {
            u32 indexed = 0, ldc = 0;
            int bindless = op->id == O_TLD4_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                bindless ? shader_direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = shader_direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return shader_untracked_handle(si, "texture", insn, i, b);
            u32 ty = op->id == O_TLD4S ? 2 : B(insn[i], 28, 3);
            u32 type = ty >> 1, array = ty & 1;
            u32 shadow = B(insn[i], 50, 1);
            int read_slot = shader_direct_add_typed_slot(textures, &texture_count, off, type, array, shadow, 0,
                                                         bindless, bank, handle_count, bank2, off2, indexed, ldc);
            if (read_slot < 0) return direct_reject(si, "texture descriptor count");
            tex_read |= 1ull << read_slot;
            tex_elsewhere |= 1ull << read_slot;
            if (a < 255) used[a] = 1;
            if (op->id == O_TLD4S) {
                if (b < 255) used[b] = 1;
                if (B(insn[i], 51, 1)) needs_gather_extended = 1;
                if (B(insn[i], 55, 1)) needs_glsl = 1;
                if (B(insn[i], 50, 1) || B(insn[i], 51, 1)) {
                    if (a + 1 < 255) used[a + 1] = 1;
                    if (B(insn[i], 50, 1) && B(insn[i], 51, 1) && b + 1 < 255) used[b + 1] = 1;
                }
                u32 db = B(insn[i], 28, 8);
                if (!B(insn[i], 55, 1) && d < 254) used[d + 1] = 1;
                if (db < 255) used[db] = 1;
                if (!B(insn[i], 55, 1) && db < 254) used[db + 1] = 1;
            } else {
                u32 coords = (type == 0 ? 1u : type == 1 ? 2u : 3u) + array;
                for (u32 k = 1; k < coords; k++)
                    if (a + k < 255) used[a + k] = 1;
                u32 extra = b + bindless;
                if (B(insn[i], bindless ? 36 : 54, 2) == 1) {
                    if (extra < 255) used[extra] = 1;
                    extra++;
                    needs_gather_extended = 1;
                } else if (B(insn[i], bindless ? 36 : 54, 2) == 2) {
                    if (extra < 255) used[extra] = 1;
                    if (extra + 1 < 255) used[extra + 1] = 1;
                    extra += 2;
                    needs_gather_extended = 1;
                }
                if (shadow && extra < 255) used[extra] = 1;
                u32 mask = B(insn[i], 31, 4), dst = d;
                for (u32 component = 0; component < 4; component++)
                    if (mask & (1u << component) && dst < 255) used[dst++] = 1;
            }
        }
        if (op->id == O_IPA && B(insn[i], 38, 1)) {
            /* indexed: any generic input the header's map gives a mode, the perspective ones through FragCoord */
            if (B(insn[i], 51, 1)) needs_glsl = 1;
            used[a] = 1;
            for (u32 v = 0; v < 32; v++)
                if ((sph[6 + v / 4] >> (v % 4 * 8)) & 0xff) {
                    inputs_used[v] = 1;
                    if (shader_ipa_interpolation(sph, v) == 2) needs_fragcoord = 1;
                }
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        } else if (op->id == O_IPA) {
            if (B(insn[i], 51, 1)) needs_glsl = 1;
            u32 addr = B(insn[i], 30, 8) * 4;
            if (addr == 0x64) {
                layer_input_used = 1;
            } else if (addr == 0x3fc) {
                needs_frontface = 1;
            } else if (addr == 0x2e0 || addr == 0x2e4) {
                needs_point_coord = 1;
            } else if (addr == 0x60) {
                primitive_id_used = 1;
            } else if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                u32 interpolation = shader_ipa_interpolation(sph, input);
                inputs_used[input] = 1;
                if (interpolation == 2) needs_fragcoord = 1;
            } else
                needs_fragcoord = 1;
            if (addr == 0x70 || addr == 0x74) needs_scale = 1;
            if (addr == 0x74) needs_frag_y = 1;
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        }
        if (op->id == O_EXIT && flow_always(insn[i])) has_exit = 1;
        if (op->id == O_KIL) needs_demote = 1;
        if (stage == ST_FS &&
            (op->id == O_TMML || op->id == O_TMML_B || (op->id == O_TEXS && !shader_texs_explicit(B(insn[i], 53, 4))) ||
             ((op->id == O_TEX || op->id == O_TEX_B) &&
              (B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 0 || B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 2 ||
               B(insn[i], op->id == O_TEX_B ? 37 : 55, 3) == 6))))
            needs_derivatives = 1;
        if (stage == ST_FS && shader_helper_side_effect(op->id)) needs_demote = 1;
        if (op->id == O_VOTE || op->id == O_SHFL) needs_subgroup = 1;
        if (op->id == O_SHFL && stage == ST_FS && g_shader_quad && shader_shfl_quad(insn[i], NULL)) needs_quad = 1;
        /* FSWZADD's lanes in a quad are the subgroup's, as SHFL's are, when they don't all do the same */
        if (op->id == O_FSWZADD && shader_fswzadd_lanes(insn[i], B(insn[i], 28, 2)) != 15) needs_subgroup = 1;
        if (op->id == O_S2R && (B(insn[i], 20, 8) == 0 || (B(insn[i], 20, 8) >= 0x38 && B(insn[i], 20, 8) <= 0x3c)))
            lane_used = 1;
    }
    /* the lane and its masks come from the subgroup, where VOTE and SHFL see the lanes. A vertex pipeline program
       without them has the lane of the batch model the translator gives it (SR_INVOCATION_INFO's primitive index 0:
       each primitive has its batch to itself), whatever the device's subgroups: a control program's invocations are
       lanes 0 up, its invocation id, and any other's is lane 0. NVN's evaluation programs read SR_LANEID (into a
       register SR_INVOCATION_INFO then overwrites), and on a device without subgroups in that stage (lavapipe) the
       program wasn't translated and its draws were lost */
    int lane_model = !needs_subgroup && stage != ST_FS && stage != ST_CS;
    int lane_is_invocation = lane_model && stage == ST_TCS;
    if (lane_used && lane_is_invocation) invocation_used = 1;
    if (lane_used && !lane_model) needs_subgroup = 1;
    if (!has_exit) return direct_reject(si, "missing exit");
    if (has_pexit && has_conditional_exit) return direct_reject(si, "conditional EXIT with PEXIT token");
    if (needs_subgroup && !can_subgroup) return direct_reject(si, "subgroup ballot/shuffle unavailable");
    if (has_flow && needs_derivatives && !can_subgroup)
        return direct_reject(si, "fragment derivative convergence unavailable");
    if (needs_f64 && !can_preserve_f64) return direct_reject(si, "FP64 float controls unavailable");
    /* The flow loop runs one block per iteration, so lanes that took the two sides of a branch reach the join on
       different iterations, and VOTE, SHFL and BAR would see part of the warp. Each iteration runs only the lanes
       at the subgroup's lowest pc, as the hardware reconverges: lanes ahead wait for the others to reach them. */
    converge = has_flow && can_subgroup && (needs_subgroup || has_barrier || needs_derivatives);
    /* FTZ per instruction: without the execution mode, or with it where some FADD, FMUL or FFMA keeps subnormals (the
       mode is the whole module's). Never on a device that flushes by itself, which also flushes those that keep them:
       the flush would change no bit */
    int soft_ftz_on = can_flush != SH_FTZ_NATIVE && (can_flush == SH_FTZ_EMULATE || fp32_preserve);
    if (needs_shared && (stage != ST_CS || si->shared_size <= 0)) return direct_reject(si, "shared memory size");
    for (int i = 0; i < texture_count; i++)
        if (textures[i].shadow && (si->tex_int_mask & (1ull << i))) return direct_reject(si, "integer shadow texture");
    u32* pop_targets = has_stack ? calloc(n, sizeof *pop_targets) : NULL;
    if (has_stack && !pop_targets) return NULL;
    int dynamic_stack = has_pexit || (has_stack && !shader_resolve_crs(insn, n, pop_targets));

    u8* gb_at = calloc(n, 1);
    if (!gb_at) {
        free(pop_targets);
        return NULL;
    }
    int gtags[255] = {0}, ngbuf = 0;
    u32 gbuf_wmask = 0;
    u32 gbuf_off[16] = {0};
    /* the first access whose base the tracking lost (0 for none), its register, and whether one of them stores */
    u32 untracked_at = 0, untracked_reg = 0;
    int untracked_store = 0;
    for (u32 i = 11; i < n; i++) {
        u32 second;
        u32 r;
        if (!IDX_OK(i)) continue;
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        int tag = 0;
        /* Direct CBE export is disabled (SR_DIRECTCBEWRITE* are zero). Track its address separately so the
           compiler's unused direct-export stores do not require a guest global-buffer descriptor. */
        if (op == O_S2R && B(w, 20, 8) == 0x15)
            tag = 0xcbe;
        else if (op == O_MOV_C) {
            tag = shader_gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = shader_gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_LDC && a == 255 && (B(w, 48, 3) == 4 || B(w, 48, 3) == 5)) {
            tag = shader_gbuf_tag(stage, B(w, 36, 5), (u32)(i32)(i16)B(w, 20, 16));
            if (!tag) tag = shader_gbuf_tag_any(stage, B(w, 36, 5), (u32)(i32)(i16)B(w, 20, 16));
        } else if (op == O_MOV_R)
            tag = b == 255 ? 0 : gtags[b];
        else if (op == O_IADD32I && a < 255)
            tag = gtags[a];
        else if (op == O_IADD_I && a < 255 && B(w, 48, 2) != 1)
            tag = gtags[a];
        else if (op == O_IADD_C) {
            tag = shader_gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag && B(w, 48, 2) != 2 && a < 255) tag = gtags[a];
            /* an index plus a bare address of the program's own, neither negated. Of two such constants added
               (an offset into the buffer, then its address), the one nearest the access is the base, as Eden's
               breadth-first search back from the access finds it: one game adds c[16][0x10], an offset, and then
               c[6][0x0], the address. An NVN descriptor's address already in Ra stays the base */
            if (!B(w, 48, 2) && (!tag || tag > 0xFFFF)) {
                int bare = shader_gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
                if (bare) tag = bare;
            }
            if (B(w, 48, 2) == 1) tag = 0;
        } else if (op == O_IADD_R) {
            tag = B(w, 48, 2) == 1      ? (a < 255 ? gtags[a] : 0)
                  : B(w, 48, 2) == 2    ? (b < 255 ? gtags[b] : 0)
                  : a < 255 && gtags[a] ? gtags[a]
                  : b < 255             ? gtags[b]
                                        : 0;
        } else if ((op == O_IADD3_I || (op == O_IADD3_R && !B(w, 31, 8))) && !B(w, 48, 4)) {
            u32 cr = B(w, 39, 8);
            tag = a < 255 && gtags[a]          ? gtags[a]
                  : cr < 255 && gtags[cr]      ? gtags[cr]
                  : op == O_IADD3_R && b < 255 ? gtags[b]
                                               : 0;
        } else if (op == O_ISCADD_C && B(w, 48, 2) != 1) {
            tag = shader_gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = shader_gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_ISCADD_R && B(w, 48, 2) != 1 && b < 255)
            tag = gtags[b];
        /* LEA's low word adds the base the same way, with its carry out for the high word's LEA.HI.X; negated
           (bit 45) or with a carry in (bit 46) it is no base plus an index */
        else if (op == O_LEA_LC && !B(w, 45, 2)) {
            tag = shader_gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag) tag = shader_gbuf_tag_any(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        } else if (op == O_LEA_LR && !B(w, 45, 2) && b < 255)
            tag = gtags[b];
        if (tag == 0xb1 && op != O_MOV_C && op != O_LDC && op != O_MOV_R) tag = 0;
        if (op == O_LDG || op == O_STG || op == O_RED || op == O_ATOM || op == O_ATOM_CAS) {
            int current = a < 255 ? gtags[a] : 0, slot = 0;
            if (!current && (op == O_LDG || op == O_STG) && B(w, 48, 3) >= 4) {
                /* the address came along a path the tracking doesn't follow (a switch's case, a loop): it is found
                   among the program's tracked buffers when it runs (shader_global_dynamic) */
                gb_at[i] = GB_DYNAMIC;
                untracked_store |= op == O_STG;
                if (!untracked_at) {
                    untracked_at = i;
                    untracked_reg = a;
                }
            } else if (!current) {
                untracked_at = i;
                untracked_reg = a;
                return shader_untracked_reject(si, insn, untracked_at, untracked_reg, gb_at, pop_targets);
            } else if (current == 0xcbe) {
                if (op != O_STG) {
                    free(gb_at);
                    free(pop_targets);
                    return direct_reject(si, "direct CBE address used for a non-export operation");
                }
                gb_at[i] = 255; /* disabled direct CBE export */
            } else {
                if (current == 0xb1) {
                    i32 byte_offset = (i32)(B(w, 20, 24) << 8) >> 8;
                    u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
                    if ((op != O_LDG && op != O_STG) || byte_offset < 0 || (u32)byte_offset + words * 4 > 16) {
                        free(gb_at);
                        free(pop_targets);
                        return direct_reject(si, "vertex pointer access exceeds known range");
                    }
                }
                u32 descriptor = (u32)(current - 1);
                for (; slot < ngbuf && gbuf_off[slot] != descriptor; slot++) {}
                if (slot == ngbuf) {
                    if (ngbuf == 16) {
                        free(gb_at);
                        free(pop_targets);
                        return direct_reject(si, "global buffer count");
                    }
                    gbuf_off[ngbuf++] = descriptor;
                }
                gb_at[i] = (u8)(slot + 1);
                if (op != O_LDG) gbuf_wmask |= 1u << slot;
                {
                    u32 bank = SH_GBUF_BANK(descriptor), at = SH_GBUF_OFFSET(descriptor);
                    cbuf_used[bank] = 1;
                    if ((at | 15u) + 1 > cbuf_max[bank]) cbuf_max[bank] = (at | 15u) + 1;
                }
            }
        }
        /* every register the instruction writes stops being a pointer, then its destination takes the new tag */
        second = op == O_TEXS || op == O_TLDS || op == O_TLD4S ? B(w, 28, 8) : 255;
        for (r = d; r < d + 4 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        for (r = second; r < second + 2 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        if (shader_writes_reg(w, op, d)) gtags[d] = tag;
    }
    /* an untracked access needs a tracked buffer to be in; one that stores may write any of them */
    for (int slot = 0; untracked_at && slot < ngbuf; slot++)
        if (gbuf_off[slot] != 0xb0) {
            untracked_at = 0;
            if (untracked_store) gbuf_wmask |= (1u << ngbuf) - 1;
        }
    if (untracked_at) return shader_untracked_reject(si, insn, untracked_at, untracked_reg, gb_at, pop_targets);

    /* A zero skip bit forwards that scalar from the input primitive. */
    if (passthrough) {
        if ((~si->gp_skip_mask[0] >> 28) & 15) position_used = position_in_used = 1;
        for (u32 a = 0; a < 32; a++)
            if ((~si->gp_skip_mask[1 + a / 8] >> ((a % 8) * 4)) & 15) inputs_used[a] = outputs_used[a] = 1;
    }

    /* the primitives of the stages between: the control program's output patch (the SPH's threads per input
       primitive), the evaluation program's domain, spacing and output (ShaderInfo.rt_int_mask), and the geometry
       program's input and output primitives */
    if (stage == ST_TCS && (!out_vertices || out_vertices > 32))
        return direct_reject_with_buffers(si, "patch output vertices", pop_targets, gb_at);
    /* a mask of viewport 0 takes the place of an index the program writes, which the translation, keeping the
       index, wouldn't */
    if (viewport_mask_one && viewport_used)
        return direct_reject_with_buffers(si, "viewport mask 1 beside a viewport index: viewport 0 or the index's",
                                          pop_targets, gb_at);
    if (viewport_used && (stage == ST_TCS || !(g_shader_viewport_index & (stage == ST_GS ? 2 : 1))))
        return direct_reject_with_buffers(si, "viewport index output unavailable", pop_targets, gb_at);
    if (stage == ST_TCS && (clip_used || layer_output_used || point_size_used))
        return direct_reject_with_buffers(si, "clip distance, layer or point size in tessellation control", pop_targets,
                                          gb_at);
    if (stage == ST_TES && ((si->rt_int_mask & 3) == 3 || ((si->rt_int_mask >> 4) & 3) == 3))
        return direct_reject_with_buffers(si, "tessellation domain or spacing", pop_targets, gb_at);
    if (stage == ST_GS && in_vertices != 1 && in_vertices != 2 && in_vertices != 3 && in_vertices != 4 &&
        in_vertices != 6)
        return direct_reject_with_buffers(si, "geometry input primitive", pop_targets, gb_at);
    if (stage == ST_GS && !passthrough && ((sph[3] >> 24) & 15) != 1 && ((sph[3] >> 24) & 15) != 6 &&
        ((sph[3] >> 24) & 15) != 7)
        return direct_reject_with_buffers(si, "geometry output topology", pop_targets, gb_at);
    if (stage == ST_GS && ((sph[4] & 0xfff) > 256 || (sph[2] >> 24) > 32))
        return direct_reject_with_buffers(si, "geometry output vertices or invocations", pop_targets, gb_at);
    /* Each distinct handle range and sampled image kind needs its own descriptor table. */
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++) {
        const ShTex* t = &textures[k_indexed];
        if (!(t->flags & SNX_TEX_INDEXED)) continue;
        if (t->flags & 3) return direct_reject_with_buffers(si, "indexed multisample texture", pop_targets, gb_at);
        indexed_shadow |= t->shadow;
        int table = 0;
        while (table < indexed_count && !shader_same_indexed_table(t, &textures[first_indexed[table]])) table++;
        if (table == indexed_count) {
            if (indexed_count == SH_INDEXED_TABLES)
                return direct_reject_with_buffers(si, "indexed texture table count", pop_targets, gb_at);
            first_indexed[indexed_count++] = k_indexed;
        }
        indexed_table[k_indexed] = table;
    }
    /* the 2D slots the key marks rectangle textures, whose samples normalize their coordinates (shader_rect_coords) */
    u64 rect_slots = 0;
    for (int k = 0; k < texture_count; k++)
        if ((si->tex_rect_mask >> k & 1) && textures[k].type == 1 && !(textures[k].flags & SNX_TEX_INDEXED))
            rect_slots |= 1ull << k;
    if (rect_slots) {
        needs_image_query = 1;
        if (stage == ST_FS) needs_texel_scale = 1; /* a scaled target's size is unscaled first */
    }
    DirectCtx* c = calloc(1, sizeof *c);
    if (!c) {
        free(gb_at);
        free(pop_targets);
        return NULL;
    }
    c->ftz_float = can_preserve;
    c->soft_ftz_on = soft_ftz_on;
    spv_emit_init(&c->emit, (size_t)n * 20 + 128);
    spv_emit_shader_capability(&c->emit);
    if (stage == ST_TCS || stage == ST_TES) spv_emit_capability(&c->emit, 3); /* Tessellation */
    if (stage == ST_GS) spv_emit_capability(&c->emit, 2);                     /* Geometry */
    if (needs_demote || (converge && stage == ST_FS)) {
        spv_emit_capability(&c->emit, 5379); /* DemoteToHelperInvocation */
        spv_emit_demote_extension(&c->emit);
    }
    if (needs_f16) spv_emit_float16_capability(&c->emit);
    if (needs_f64) spv_emit_float64_capability(&c->emit);
    if (needs_clock) spv_emit_shader_clock_extension(&c->emit);
    if (needs_i64) spv_emit_capability(&c->emit, 11); /* Int64 */
    if ((needs_instance || needs_draw_params) && g_shader_draw_parameters && stage == ST_VS)
        spv_emit_draw_parameters_capability(&c->emit);
    if (needs_subgroup || converge) spv_emit_subgroup_capabilities(&c->emit);
    if (needs_quad) spv_emit_subgroup_quad_capability(&c->emit);
    c->quad = needs_quad;
    if (converge) spv_emit_subgroup_arithmetic_capability(&c->emit);
    if (needs_gather_extended) spv_emit_image_gather_extended_capability(&c->emit);
    if (needs_image_query) spv_emit_image_query_capability(&c->emit);
    /* the per-instruction flush to zero tests the absolute value, GLSL.std.450's FAbs, where the module keeps signed
       zeros (shader_float_ftz): without the import its OpExtInst named id 0, which NVIDIA's SPIR-V front end (615.78)
       crashed on, a shader of FADD and FMUL with nothing else from the set */
    if (fp32_flush && soft_ftz_on && can_preserve) needs_glsl = 1;
    if (needs_glsl) c->glsl = spv_emit_glsl450_import(&c->emit);
    u32 fn = spv_emit_id(&c->emit), label = spv_emit_id(&c->emit);
    u32 void_type = spv_emit_type_void(&c->emit);
    c->u32_type = spv_emit_type_u32(&c->emit);
    u32 i32_type = spv_emit_type_i32(&c->emit);
    c->f32_type = spv_emit_type_f32(&c->emit);
    if (needs_f16) c->f16_type = spv_emit_type_f16(&c->emit);
    if (needs_f64) c->f64_type = spv_emit_type_f64(&c->emit);
    u32 i64_type = needs_i64 ? spv_emit_type_int64(&c->emit, 1) : 0,
        u64_type = needs_i64 ? spv_emit_type_int64(&c->emit, 0) : 0;
    c->bool_type = spv_emit_type_bool(&c->emit);
    c->true_value = spv_emit_constant_bool(&c->emit, c->bool_type, 1);
    c->false_value = spv_emit_constant_bool(&c->emit, c->bool_type, 0);
    u32 vec4 = spv_emit_type_vec4(&c->emit, c->f32_type);
    u32 vec4_u32 = spv_emit_type_vec4(&c->emit, c->u32_type);
    u32 vec4_i32 = spv_emit_type_vec4(&c->emit, i32_type);
    u32 vec2 = spv_emit_type_vec2(&c->emit, c->f32_type);
    u32 vec2f16 = needs_f16 ? spv_emit_type_vec2(&c->emit, c->f16_type) : 0;
    u32 vec2_u32 = spv_emit_type_vec2(&c->emit, c->u32_type);
    u32 vec2_i32 = spv_emit_type_vec2(&c->emit, i32_type);
    u32 vec3 = spv_emit_type_vec3(&c->emit, c->f32_type);
    u32 vec3_u32 = spv_emit_type_vec3(&c->emit, c->u32_type);
    u32 vec3_i32 = spv_emit_type_vec3(&c->emit, i32_type);
    u32 fn_type = spv_emit_type_function_void(&c->emit, void_type);
    u32 reg_ptr = spv_emit_type_function_pointer(&c->emit, c->u32_type);
    c->zero = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 zero_f = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    /* Maximum VS interface: point size, position, 32 outputs, 32 inputs, 18 buffers, 32 samplers, push constants. */
    u32 outputs[8], inputs[32] = {0}, input_vec_type[32] = {0}, input_component_type[32] = {0};
    u32 output_attrs[32] = {0}, position = 0, point_size = 0, layer_output = 0, layer_input = 0;
    u32 viewport_output = 0;
    /* transform feedback: each output word's buffer (0xFF none) and offset, the outputs written to a private vec4,
       and their component outputs (shader_xfb_store) */
    u8 xfb_buf[160], attr_private[32] = {0};
    u16 xfb_off[160] = {0};
    u32 xfb_components[32][4] = {{0}};
    int xfb_on = 0;
    memset(xfb_buf, 0xFF, sizeof xfb_buf);
    u32 clip_output = 0, clip_count = 0;
    u32 depth_output = 0, interfaces[384], count = 0;
    /* the stages between: the levels and vectors of a patch, and the inputs of each vertex */
    u32 tess_outer = 0, tess_inner = 0, patch_vars[30] = {0}, position_in = 0, primitive_id = 0, primitive_id_out = 0,
        tess_coord = 0;
    u32 invocation_id = 0, patch_vertices = 0;
    int depth_fix = position_used && last && stage != ST_FS && stage != ST_CS;
    /* BuiltIn Layer takes the Geometry capability (a geometry program has it; the device's geometryShader) or
       ShaderLayer (its shaderOutputLayer), which a vertex or tessellation program writing the layer needs. A fragment
       program reads it with either. Without one a write is dropped and a read is 0 */
    if (layer_output_used && stage != ST_GS && !g_shader_output_layer) layer_output_used = 0;
    if (layer_input_used && !g_shader_geometry && !g_shader_output_layer) layer_input_used = 0;
    if ((layer_output_used && stage != ST_GS) || (layer_input_used && !g_shader_geometry))
        spv_emit_layer_capability(&c->emit);
    else if (layer_input_used && stage != ST_GS)
        spv_emit_capability(&c->emit, 2); /* Geometry */
    if (clip_used) spv_emit_clip_distance_capability(&c->emit);
    if (stage == ST_FS)
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                u32 color_type =
                    (si->rt_int_mask & (1u << rt)) ? (si->rt_sint_mask & (1u << rt)) ? vec4_i32 : vec4_u32 : vec4;
                outputs[count] = spv_emit_output_vec4(&c->emit, color_type, rt);
                interfaces[count] = outputs[count];
                count++;
            }
    if (stage == ST_FS && (sph[19] & 2u)) {
        depth_output = spv_emit_frag_depth_output(&c->emit, c->f32_type);
        interfaces[count++] = depth_output;
    }
    /* The guest's SampleMask output is the host's: a render target has the guest's samples (vk/msaa.c), bit i
       sample i, and a single-sampled one only bit 0. PIXLD.COVMASK likewise reports the host's coverage */
    u32 sample_mask_output = 0; /* SampleMask, an array of one word */
    if (stage == ST_FS && (sph[19] & 1u)) {
        sample_mask_output = spv_emit_interface_variable(&c->emit, 3, c->u32_type, c->u32_type, 1, 20, 0, 0);
        interfaces[count++] = sample_mask_output;
    }
    if (stage != ST_FS && stage != ST_CS) {
        /* a tessellation control program's outputs are arrays of its output vertices */
        u32 per_vertex = stage == ST_TCS ? out_vertices : 0;
        if (stage == ST_VS || (stage != ST_TCS && g_shader_stage_point_size)) {
            if (stage == ST_TES) spv_emit_capability(&c->emit, 23); /* TessellationPointSize */
            if (stage == ST_GS) spv_emit_capability(&c->emit, 24);  /* GeometryPointSize */
            point_size = spv_emit_point_size_output(&c->emit, c->f32_type);
            interfaces[count++] = point_size;
        }
        if (viewport_used) {
            spv_emit_capability(&c->emit, stage == ST_GS ? 57 : 70); /* MultiViewport, ShaderViewportIndex */
            viewport_output = spv_emit_interface_variable(&c->emit, 3, i32_type, c->u32_type, 0, 10, 0, 0);
            interfaces[count++] = viewport_output;
        }
        if (layer_output_used) {
            layer_output = spv_emit_layer_output(&c->emit, i32_type);
            interfaces[count++] = layer_output;
        }
        if (clip_used) {
            u32 i;
            for (i = 0; i < 8; i++)
                if (clip_used & (1u << i)) clip_count = i + 1;
            clip_output = spv_emit_clip_distance_output(&c->emit, c->f32_type, c->u32_type, clip_count);
            interfaces[count++] = clip_output;
        }
        /* transform feedback, in the last stage before rasterization: the buffer and offset of each output word the
           key captures (a word captured twice goes to its first place) */
        if (!per_vertex && !(si->clip_enable & SNX_SH_NOT_LAST))
            for (u32 b = 0; b < 4; b++)
                for (u32 w = 0; w < si->xfb.count[b] && w < 128; w++) {
                    u32 word = si->xfb.layout[b][w];
                    if (shader_legacy_group(word * 4) >= 0)
                        word = shader_legacy_address(word * 4) / 4; /* where it moved */
                    if (word >= 160 || xfb_buf[word] != 0xFF) continue;
                    xfb_buf[word] = (u8)b;
                    xfb_off[word] = (u16)(w * 4);
                    xfb_on = 1;
                }
        if (position_used) {
            position = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex, 0, 0, 0)
                                  : spv_emit_position_output(&c->emit, vec4);
            interfaces[count++] = position;
            /* the position is a built-in: captured only whole, its four words in order in one buffer */
            if (xfb_buf[28] != 0xFF && xfb_buf[29] == xfb_buf[28] && xfb_buf[30] == xfb_buf[28] &&
                xfb_buf[31] == xfb_buf[28] && xfb_off[29] == xfb_off[28] + 4 && xfb_off[30] == xfb_off[28] + 8 &&
                xfb_off[31] == xfb_off[28] + 12)
                spv_emit_xfb_capture(&c->emit, position, xfb_buf[28], si->xfb.stride[xfb_buf[28]], xfb_off[28]);
        }
        for (u32 i = 0; i < 32; i++)
            if (outputs_used[i]) {
                u32 k;
                int captured = 0;
                for (k = 0; k < 4; k++) captured |= xfb_buf[32 + 4 * i + k] != 0xFF;
                if (captured) {
                    /* a captured output is written to a private vec4 and copied out a component a variable
                       (shader_xfb_store), so that each captured component has an offset of its own: the next stage's vec4
                       input takes the four at its location */
                    output_attrs[i] = spv_emit_private_variable(&c->emit, vec4);
                    interfaces[count++] = output_attrs[i];
                    attr_private[i] = 1;
                    for (k = 0; k < 4; k++) {
                        u32 word = 32 + 4 * i + k;
                        xfb_components[i][k] = spv_emit_output_component(&c->emit, c->f32_type, i, k);
                        interfaces[count++] = xfb_components[i][k];
                        if (xfb_buf[word] != 0xFF)
                            spv_emit_xfb_capture(&c->emit, xfb_components[i][k], xfb_buf[word],
                                                 si->xfb.stride[xfb_buf[word]], xfb_off[word]);
                    }
                    continue;
                }
                output_attrs[i] = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex,
                                                                           SPV_NOT_BUILTIN, i, 0)
                                             : spv_emit_output_vec4(&c->emit, vec4, i);
                interfaces[count++] = output_attrs[i];
            }
    }
    if (stage == ST_TCS || stage == ST_TES) {
        /* a patch's levels and vectors: the control program's outputs, the evaluation program's inputs */
        u32 storage = stage == ST_TCS ? 3 : 1, i;
        if (levels_used & 1) {
            tess_outer = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 4, 11, 0, 1);
            interfaces[count++] = tess_outer;
        }
        if (levels_used & 2) {
            tess_inner = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 2, 12, 0, 1);
            interfaces[count++] = tess_inner;
        }
        for (i = 0; i < 30; i++)
            if (patch_used[i]) {
                patch_vars[i] =
                    spv_emit_interface_variable(&c->emit, storage, vec4, c->u32_type, 0, SPV_NOT_BUILTIN, i, 1);
                interfaces[count++] = patch_vars[i];
            }
        patch_vertices = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 14, 0, 0);
        interfaces[count++] = patch_vertices;
    }
    /* PIXLD's sample (SampleId, which shades each sample, with SampleRateShading) and coverage (SampleMask in) */
    u32 sample_id = 0, coverage = 0;
    if (needs_sample_id) {
        spv_emit_capability(&c->emit, 35); /* SampleRateShading */
        sample_id = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 18);
        interfaces[count++] = sample_id;
    }
    if (needs_coverage) {
        coverage = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 1, 20, 0, 0);
        spv_emit_flat(&c->emit, coverage); /* an integer fragment input */
        interfaces[count++] = coverage;
    }
    /* Fragment PrimitiveId can declare Geometry or Tessellation. Without either feature the read is zero. */
    if (stage == ST_FS && primitive_id_used && (g_shader_geometry || g_shader_tessellation)) {
        if (!g_shader_geometry || !layer_input_used) spv_emit_capability(&c->emit, g_shader_geometry ? 2 : 3);
        primitive_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 7, 0, 0);
        spv_emit_flat(&c->emit, primitive_id);
        interfaces[count++] = primitive_id;
    }
    if (between) {
        if (position_in_used) {
            position_in = spv_emit_interface_variable(&c->emit, 1, vec4, c->u32_type, in_vertices, 0, 0, 0);
            interfaces[count++] = position_in;
        }
        if (primitive_id_used || stage == ST_GS) {
            primitive_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 7, 0, 0);
            interfaces[count++] = primitive_id;
        }
        if (stage == ST_GS) {
            /* Vulkan requires a geometry shader to write PrimitiveId for a later fragment shader to read it. */
            primitive_id_out = spv_emit_interface_variable(&c->emit, 3, c->u32_type, c->u32_type, 0, 7, 0, 0);
            interfaces[count++] = primitive_id_out;
        }
        if (tess_coord_used) {
            tess_coord = spv_emit_interface_variable(&c->emit, 1, vec3, c->u32_type, 0, 13, 0, 0);
            interfaces[count++] = tess_coord;
        }
        if (invocation_used) {
            invocation_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 8, 0, 0);
            interfaces[count++] = invocation_id;
        }
    }
    for (u32 i = 0; i < 32; i++) {
        if (!inputs_used[i]) continue;
        if (stage == ST_FS && (si->in_unfed_mask >> i & 1)) continue; /* read as 0, 0, 0, 1 (ShaderInfo) */
        u32 interpolation = 0;
        if (stage == ST_FS) {
            u32 mode_byte = (sph[6 + i / 4] >> (i % 4 * 8)) & 0xFF; /* the header's byte 0x18 + i */
            u32 k = 0;
            while (k < 3 && !((mode_byte >> (k * 2)) & 3)) k++;
            u32 mode = (mode_byte >> (k * 2)) & 3;
            interpolation = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        }
        u32 component_type = c->f32_type;
        u32 vector_type = vec4;
        if (stage == ST_VS && (si->attr_int_mask & (1u << i))) {
            int signed_input = (si->attr_sint_mask & (1u << i)) != 0;
            component_type = signed_input ? i32_type : c->u32_type;
            vector_type = signed_input ? vec4_i32 : vec4_u32;
        }
        input_vec_type[i] = vector_type;
        input_component_type[i] = component_type;
        inputs[i] = between ? spv_emit_interface_variable(&c->emit, 1, vector_type, c->u32_type, in_vertices,
                                                          SPV_NOT_BUILTIN, i, 0)
                            : spv_emit_input_vec4(&c->emit, vector_type, i, interpolation);
        interfaces[count++] = inputs[i];
    }
    u32 fragcoord = 0;
    if (layer_input_used) {
        layer_input = spv_emit_builtin_input_u32(&c->emit, i32_type, 9);
        interfaces[count++] = layer_input;
    }
    u32 frontface = 0;
    if (needs_frontface) {
        frontface = spv_emit_builtin_input_u32(&c->emit, c->bool_type, 17);
        interfaces[count++] = frontface;
    }
    /* a[0x2e0] and a[0x2e4] are the point sprite's S and T */
    u32 point_coord = 0;
    if (needs_point_coord) {
        point_coord = spv_emit_interface_variable(&c->emit, 1, vec2, c->u32_type, 0, 16, 0, 0);
        interfaces[count++] = point_coord;
    }
    u32 instance_index = 0, vertex_index = 0, base_instance = 0;
    u32 local_input = 0, group_input = 0;
    if (needs_local_id) {
        local_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 27);
        interfaces[count++] = local_input;
    }
    if (needs_group_id) {
        group_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 26);
        interfaces[count++] = group_input;
    }
    if (needs_subgroup) {
        c->subgroup_lane_input = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 41);
        if (stage == ST_FS) spv_emit_flat(&c->emit, c->subgroup_lane_input);
        interfaces[count++] = c->subgroup_lane_input;
    }
    if (needs_instance) {
        instance_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 43);
        vertex_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 42);
        interfaces[count++] = instance_index;
        interfaces[count++] = vertex_index;
        if (g_shader_draw_parameters && stage == ST_VS) {
            base_instance = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 4425); /* BaseInstance */
            interfaces[count++] = base_instance;
        }
    }
    u32 base_vertex = 0;
    if (needs_draw_params && g_shader_draw_parameters && stage == ST_VS) {
        if (!base_instance) {
            base_instance = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 4425); /* BaseInstance */
            interfaces[count++] = base_instance;
        }
        base_vertex = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 4424); /* BaseVertex */
        interfaces[count++] = base_vertex;
    }
    if (needs_fragcoord) {
        fragcoord = spv_emit_fragcoord_input(&c->emit, vec4);
        interfaces[count++] = fragcoord;
    }
    if (needs_scale || needs_texel_scale) {
        c->scale_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4, c->f32_type, c->u32_type, 1, SNX_VKB_RS, 1, &c->scale_pointer);
        interfaces[count++] = c->scale_buffer;
    }
    for (u32 i = 0; i < 18; i++) {
        if (!cbuf_used[i]) continue;
        c->cbuf[i] = spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage), i,
                                                 4096, &c->cbuf_pointer[i]);
        interfaces[count++] = c->cbuf[i];
    }
    if (indexed_shadow) {
        c->tex_func_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage),
                                        SNX_VKB_TEXFUNCS, SNX_TEX_ARRAY / 4, &c->tex_func_pointer);
        interfaces[count++] = c->tex_func_buffer;
    }
    if (indexed_count) {
        c->tex_map_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage),
                                        SNX_VKB_TEXMAP, SH_INDEXED_TABLES * SNX_TEX_ARRAY / 4, &c->tex_map_pointer);
        interfaces[count++] = c->tex_map_buffer;
    }
    u32 texture_scalar[SH_MAXTEX], texture_vec4[SH_MAXTEX];
    for (int i = 0; i < texture_count; i++) {
        u32 scalar = (textures[i].flags & 1)            ? c->u32_type
                     : (si->tex_int_mask & (1ull << i)) ? (si->tex_sint_mask & (1ull << i)) ? i32_type : c->u32_type
                                                        : c->f32_type;
        texture_scalar[i] = scalar;
        texture_vec4[i] = scalar == i32_type ? vec4_i32 : scalar == c->u32_type ? vec4_u32 : vec4;
        if (textures[i].flags & SNX_TEX_INDEXED) {
            c->tex_table[i] = (u8)indexed_table[i];
            int same = 0;
            while (same < i && !((textures[same].flags & SNX_TEX_INDEXED) &&
                                 shader_same_indexed_kind(textures, si->tex_int_mask, si->tex_sint_mask, same, i)))
                same++;
            if (same == i) { /* the first slot of its kind declares the kind's arrays */
                c->tex_array[i] =
                    spv_emit_sampler_image_array(&c->emit, scalar, c->u32_type, shader_vk_set(stage), SNX_VKB_TEXARRAY,
                                                 textures[i].type == 0 ? 1 : textures[i].type, textures[i].array, 0,
                                                 SNX_TEX_ARRAY, &c->sampled_type[i]);
                interfaces[count++] = c->tex_array[i];
                if (textures[i].shadow) {
                    c->tex_depth[i] =
                        spv_emit_sampler_image_array(&c->emit, c->f32_type, c->u32_type, shader_vk_set(stage),
                                                     SNX_VKB_TEXDEPTH, textures[i].type == 0 ? 1 : textures[i].type,
                                                     textures[i].array, 1, SNX_TEX_ARRAY, &c->tex_depth_type[i]);
                    interfaces[count++] = c->tex_depth[i];
                }
            } else {
                c->tex_array[i] = c->tex_array[same];
                c->tex_depth[i] = c->tex_depth[same];
                c->tex_depth_type[i] = c->tex_depth_type[same];
                c->sampled_type[i] = c->sampled_type[same];
            }
            c->sampler[i] = c->tex_array[i];
            continue;
        }
        c->sampler[i] = spv_emit_sampler_image(&c->emit, scalar, shader_vk_set(stage), SNX_VKB_TEX + (u32)i,
                                               textures[i].type == 0 ? 1 : textures[i].type, textures[i].array,
                                               textures[i].shadow && shader_dc_color_func(si, i) < 0,
                                               !!(textures[i].flags & 2), &c->sampled_type[i]);
        interfaces[count++] = c->sampler[i];
    }
    /* Formatted stores follow the bound view's format. Only loads and atomics
       require R32ui; declaring stores R32ui loses the other components. */
    for (int i = 0; i < image_info.nimg; i++) {
        int integer = image_info.img[i].kind != 0, buffer = image_info.img[i].type == 1;
        c->image[i] = spv_emit_storage_image(&c->emit,
                                             image_info.img[i].kind == 3 ? i32_type
                                             : integer                   ? c->u32_type
                                                                         : c->f32_type,
                                             shader_vk_set(stage), (buffer ? SNX_VKB_IMGBUF : SNX_VKB_IMG) + (u32)i,
                                             buffer                        ? 5
                                             : image_info.img[i].type == 5 ? 2
                                                                           : 1,
                                             image_info.img[i].type == 4, image_info.img[i].kind == 1 ? 33 : 0,
                                             &c->image_type[i]);
        interfaces[count++] = c->image[i];
    }
    if (ngbuf) {
        u32 array = spv_emit_type_runtime_array(&c->emit, c->u32_type, 4);
        u32 structure = spv_emit_type_struct_one(&c->emit, array);
        c->gbuf_pointer = spv_emit_type_storage_pointer(&c->emit, c->u32_type);
        for (int i = 0; i < ngbuf; i++) {
            c->gbuf[i] = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SSBO + (u32)i);
            interfaces[count++] = c->gbuf[i];
        }
        if (stage != ST_CS) {
            c->smap = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SMAP);
            interfaces[count++] = c->smap;
        }
    }
    u32 shared_array = 0, shared_element_pointer = 0, shared_words = 0;
    if (needs_shared) {
        shared_words = ((u32)si->shared_size + 3) / 4;
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, shared_words);
        u32 pointer_type = spv_emit_type_workgroup_pointer(&c->emit, array_type);
        shared_element_pointer = spv_emit_type_workgroup_pointer(&c->emit, c->u32_type);
        shared_array = spv_emit_workgroup_variable(&c->emit, pointer_type);
        interfaces[count++] = shared_array;
    }
    u32 push = 0, push_pointer = 0;
    if ((stage == ST_FS && ((target & 15) || needs_point_coord || needs_texel_scale || needs_frag_y)) || depth_fix ||
        needs_instance || base_vertex) {
        /* the fragment stage's uvec4: its scaled textures (shader_scaled_coords) and the window's origin (shader_frag_y_origin) */
        push = spv_emit_push_vec4(&c->emit, vec4, c->f32_type, stage == ST_FS ? 16 : 0,
                                  stage == ST_FS && (needs_texel_scale || needs_frag_y) ? vec4_u32 : 0, &push_pointer);
        interfaces[count++] = push;
    }
    if (base_vertex) {
        c->dp_push = push;
        c->dp_push_pointer = push_pointer;
        c->dp_base_vertex = base_vertex;
        c->dp_base_instance = base_instance;
    }
    spv_emit_entry_str(&c->emit, (SpvShaderStage)stage, fn, "main", interfaces, count); /* ST_ is SPIR-V's model */
    if (xfb_on) {
        spv_emit_capability(&c->emit, 53);               /* TransformFeedback */
        spv_emit_execution_mode(&c->emit, fn, 11, 0, 0); /* Xfb */
    }
    if (stage == ST_TCS) spv_emit_execution_mode(&c->emit, fn, 26, 1, out_vertices); /* OutputVertices */
    if (stage == ST_TES) {
        /* 0xC8: the domain (isolines, triangles, quads), the spacing (equal, fractional odd and even) and the output
           (points, lines, triangles clockwise and counterclockwise) */
        u32 domain = si->rt_int_mask & 3, spacing = (si->rt_int_mask >> 4) & 3, output = (si->rt_int_mask >> 8) & 3;
        spv_emit_execution_mode(&c->emit, fn, domain == 0 ? 25 : domain == 1 ? 22 : 24, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, spacing == 0 ? 1 : spacing == 1 ? 3 : 2, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, output == 2 ? 4 : 5, 0, 0); /* VertexOrderCw, VertexOrderCcw */
        if (output == 0) spv_emit_execution_mode(&c->emit, fn, 10, 0, 0); /* PointMode */
    }
    if (stage == ST_GS) {
        /* the input primitive, the SPH's output topology and vertices, and its threads per input primitive */
        u32 topology = passthrough ? (in_vertices == 1                       ? 1
                                      : in_vertices == 2 || in_vertices == 4 ? 6
                                                                             : 7)
                                   : (sph[3] >> 24) & 15;
        u32 invocations = sph[2] >> 24;
        spv_emit_execution_mode(&c->emit, fn,
                                in_vertices == 1   ? 19
                                : in_vertices == 2 ? 20
                                : in_vertices == 4 ? 21
                                : in_vertices == 6 ? 23
                                                   : 22,
                                0, 0);
        spv_emit_execution_mode(&c->emit, fn, topology == 1 ? 27 : topology == 6 ? 28 : 29, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, 26, 1,
                                passthrough        ? (in_vertices == 4   ? 2
                                                      : in_vertices == 6 ? 3
                                                                         : in_vertices)
                                : (sph[4] & 0xfff) ? sph[4] & 0xfff
                                                   : 1);
        spv_emit_execution_mode(&c->emit, fn, 0, 1, invocations ? invocations : 1); /* Invocations */
    }
    if (stage == ST_CS) spv_emit_local_size(&c->emit, fn, si->cs_local[0], si->cs_local[1], si->cs_local[2]);
    if (stage == ST_FS) spv_emit_fragment_origin_upper_left(&c->emit, fn);
    if (stage == ST_FS && si->early_fragment_tests)
        spv_emit_execution_mode(&c->emit, fn, 9, 0, 0); /* EarlyFragmentTests */
    if (depth_output) spv_emit_fragment_depth_replacing(&c->emit, fn);
    spv_emit_float_controls(&c->emit, fn, fp32_flush && !fp32_preserve && can_flush == SH_FTZ_MODE, can_preserve,
                            needs_f64 && can_preserve_f64);
    spv_emit_function_void(&c->emit, void_type, fn, fn_type);
    spv_emit_label(&c->emit, label);
    c->emit.block_end = shader_var_flush;
    c->emit.block_end_user = c;
    u32 local_array = 0, local_element_pointer = 0, local_words = 0;
    if (needs_local) {
        u32 bytes = (sph[1] & 0xffffffu) + (sph[2] & 0xffffffu);
        local_words = 1024;
        if (bytes) {
            local_words = 1;
            while (local_words < 65536 && local_words * 4 < bytes) local_words <<= 1;
        }
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, local_words);
        u32 array_pointer = spv_emit_type_function_pointer(&c->emit, array_type);
        local_element_pointer = spv_emit_type_function_pointer(&c->emit, c->u32_type);
        local_array = spv_emit_function_variable(&c->emit, array_pointer);
    }
    for (u32 r = 0; r < 255; r++)
        if (used[r]) c->reg[r] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    /* On the per-instruction FTZ path, the registers only FTZ float instructions write hold flushed values (from zero,
       their first value): their reads need no flush. Anything else that may write a register taints it, its
       destinations taken wide (d to d+3, and the second field texture instructions write, bits 28 to 35) */
    if (soft_ftz_on) {
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            if (shader_ftz_writer(insn[i])) continue;
            for (u32 r = 0; r < 255; r++)
                if (shader_may_write(insn[i], r)) c->tainted[r] = 1;
        }
        c->taint_known = 1;
    }
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
        if (textures[k_indexed].flags & SNX_TEX_INDEXED)
            c->tex_index[k_indexed] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    u32 pred_ptr = spv_emit_type_function_pointer(&c->emit, c->bool_type);
    for (u32 p = 0; p < 7; p++)
        if (pred_used[p]) c->pred[p] = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_c = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_o = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_z = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_s = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    u32 atomic_old_var = needs_atomic_cas ? spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero) : 0;
    u32 stack_ptr = 0, stack[32] = {0};
    if (dynamic_stack) {
        stack_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 call_ptr = 0, call_stack[32] = {0};
    if (has_calls) {
        call_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) call_stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 pc = 0, loop_header = 0, loop_dispatch = 0, dispatch_default = 0, dispatch_merge = 0, loop_continue = 0,
        loop_merge = 0;
    u32 *case_labels = NULL, *case_literals = NULL;
    u8* block_start = NULL;
    size_t case_count = 0;
    ShStructure* structure = NULL;
    StructEmit se = {0};
    if (has_flow) {
        /* one case for each instruction, and the case of lanes waiting to reconverge */
        case_labels = calloc(n + 1, sizeof *case_labels + sizeof *case_literals + 1);
        if (!case_labels) abort();
        case_literals = case_labels + n + 1;
        block_start = (u8*)(case_literals + n + 1);
        block_start[11] = 1;
        int unknown_brx = 0;
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            int id = decode(insn[i])->id;
            if (id == O_BRX) {
                u32 targets[64];
                int count = shader_brx_targets(insn, i, n, targets, 64);
                if (count < 0)
                    unknown_brx = 1;
                else
                    for (int k = 0; k < count; k++) block_start[targets[k]] = 1;
            }
            if (id == O_BRA || id == O_CAL || id == O_PRET || id == O_PEXIT || id == O_SSY || id == O_PBK ||
                id == O_PCNT)
                block_start[bra_target(insn[i], i)] = 1;
            if (!dynamic_stack && (id == O_SYNC || id == O_BRK || id == O_CONT)) block_start[pop_targets[i]] = 1;
            if (id == O_BRA || id == O_CAL || id == O_RET || id == O_BRX || id == O_SYNC || id == O_BRK ||
                id == O_CONT || id == O_EXIT) {
                u32 next = shader_next_instruction(i, n);
                if (next < n) block_start[next] = 1;
            }
        }
        /* Unknown jump tables retain a case for every possible instruction target. */
        if (unknown_brx)
            for (u32 i = 11; i < n; i++)
                if (IDX_OK(i)) block_start[i] = 1;
        /* The program's own branches and loops as structured constructs, where its control flow allows them: the
           dispatch loop below makes every register live around one big loop, and costs a driver as much to compile
           again as the program's code. Lanes that must reconverge for subgroup operations, barriers or derivatives
           take them where the device reconverges them at each construct's merge (MaximallyReconvergesKHR), as the
           hardware does at its SYNC targets, and keep the loop otherwise, which runs the lowest pc first */
        if (!dynamic_stack && !has_calls && (!converge || g_shader_maximal_reconvergence) && !unknown_brx)
            structure = shader_structure(insn, n, block_start, pop_targets);
        if (structure && converge) { /* the guarantee those lanes need */
            spv_emit_extension(&c->emit, "SPV_KHR_maximal_reconvergence");
            spv_emit_execution_mode(&c->emit, fn, 6023, 0, 0); /* MaximallyReconvergesKHR */
        }
        if (structure) {
            se.st = structure;
            se.open = 1; /* the function's first block */
            se.label = calloc(4 * (size_t)structure->nscopes + 2 * (size_t)structure->ntokens, sizeof *se.label + 1);
            if (!se.label) abort();
            se.used = (u8*)(se.label + 4 * (size_t)structure->nscopes + 2 * (size_t)structure->ntokens);
        }
    }
    /* every OpVariable comes before the first block's other instructions, the clip distances' stores included */
    if (has_flow && !structure)
        pc = spv_emit_function_variable_init(&c->emit, reg_ptr, spv_emit_constant_u32(&c->emit, c->u32_type, 11));
    if (structure && structure->uses_flag) se.flag = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    c->discarded = has_flow && stage == ST_FS ? spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value) : 0;
    for (clip = 0; clip < clip_count; clip++) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, clip);
        u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output, index);
        spv_emit_store(&c->emit, ptr, zero_f);
    }
    if (has_flow && !structure) {
        loop_header = spv_emit_id(&c->emit);
        loop_dispatch = spv_emit_id(&c->emit);
        dispatch_default = spv_emit_id(&c->emit);
        dispatch_merge = spv_emit_id(&c->emit);
        loop_continue = spv_emit_id(&c->emit);
        loop_merge = spv_emit_id(&c->emit);
        for (u32 i = 11; i < n; i++)
            if (IDX_OK(i) && block_start[i]) {
                case_literals[case_count] = i;
                case_labels[case_count++] = spv_emit_id(&c->emit);
            }
        if (converge) {
            wait_label = spv_emit_id(&c->emit);
            case_literals[case_count] = UINT32_MAX - 1;
            case_labels[case_count++] = wait_label;
        }
        if (stage == ST_VS || stage == ST_TES || stage == ST_GS) {
            u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
            if (point_size) spv_emit_store(&c->emit, point_size, one_f);
            u32 components[4] = {zero_f, zero_f, zero_f, one_f};
            u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
            if (position) spv_emit_store(&c->emit, position, empty);
            for (u32 i = 0; i < 32; i++)
                if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
        }
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_header);
        spv_emit_loop_merge(&c->emit, loop_merge, loop_continue);
        spv_emit_branch(&c->emit, loop_dispatch);
        spv_emit_label(&c->emit, loop_dispatch);
        u32 selector = spv_emit_load(&c->emit, c->u32_type, pc);
        if (converge) {
            u32 lowest = spv_emit_subgroup_umin_u32(&c->emit, c->u32_type, selector);
            /* Helper lanes may be inactive for subgroup arithmetic. Their minimum may be undefined; they must
               continue at their own pc so they can still provide derivative values to their quad. */
            if (stage == ST_FS)
                lowest = spv_emit_select(&c->emit, c->u32_type, spv_emit_is_helper(&c->emit, c->bool_type), selector,
                                         lowest);
            lowest = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, selector, lowest),
                                     selector, lowest);
            selector =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, selector, lowest),
                                selector, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX - 1));
        }
        spv_emit_selection_merge(&c->emit, dispatch_merge);
        spv_emit_switch_u32(&c->emit, selector, dispatch_default, case_literals, case_labels, case_count);
    }
    if ((stage == ST_VS || stage == ST_TES || stage == ST_GS) && (!has_flow || structure)) {
        u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        if (point_size) spv_emit_store(&c->emit, point_size, one_f);
        u32 components[4] = {zero_f, zero_f, zero_f, one_f};
        u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
        if (position) spv_emit_store(&c->emit, position, empty);
        for (u32 i = 0; i < 32; i++)
            if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
    }

    size_t case_index = 0;
    const Tr tr = {
        .si = si,
        .stage = stage,
        .insn = insn,
        .n = n,
        .sph = sph,
        .target = target,
        .passthrough = passthrough,
        .point_size_used = point_size_used,
        .between = between,
        .in_vertices = in_vertices,
        .out_vertices = out_vertices,
        .clip_used = clip_used,
        .textures = textures,
        .texture_count = texture_count,
        .image_info = &image_info,
        .needs_scale = needs_scale,
        .needs_texel_scale = needs_texel_scale,
        .has_pexit = has_pexit,
        .lane_model = lane_model,
        .lane_is_invocation = lane_is_invocation,
        .pop_targets = pop_targets,
        .dynamic_stack = dynamic_stack,
        .gb_at = gb_at,
        .ngbuf = ngbuf,
        .gbuf_off = gbuf_off,
        .rect_slots = rect_slots,
        .c = c,
        .i32_type = i32_type,
        .i64_type = i64_type,
        .u64_type = u64_type,
        .vec4 = vec4,
        .vec4_u32 = vec4_u32,
        .vec4_i32 = vec4_i32,
        .vec2 = vec2,
        .vec2f16 = vec2f16,
        .vec2_u32 = vec2_u32,
        .vec2_i32 = vec2_i32,
        .vec3 = vec3,
        .vec3_u32 = vec3_u32,
        .vec3_i32 = vec3_i32,
        .zero_f = zero_f,
        .inputs = inputs,
        .outputs = outputs,
        .input_component_type = input_component_type,
        .input_vec_type = input_vec_type,
        .position = position,
        .output_attrs = output_attrs,
        .point_size = point_size,
        .layer_output = layer_output,
        .layer_input = layer_input,
        .viewport_output = viewport_output,
        .attr_private = attr_private,
        .xfb_components = xfb_components,
        .xfb_on = xfb_on,
        .clip_output = clip_output,
        .clip_count = clip_count,
        .depth_output = depth_output,
        .position_in = position_in,
        .primitive_id = primitive_id,
        .primitive_id_out = primitive_id_out,
        .patch_vars = patch_vars,
        .tess_outer = tess_outer,
        .tess_inner = tess_inner,
        .tess_coord = tess_coord,
        .invocation_id = invocation_id,
        .patch_vertices = patch_vertices,
        .depth_fix = depth_fix,
        .sample_mask_output = sample_mask_output,
        .sample_id = sample_id,
        .coverage = coverage,
        .fragcoord = fragcoord,
        .frontface = frontface,
        .point_coord = point_coord,
        .instance_index = instance_index,
        .base_instance = base_instance,
        .vertex_index = vertex_index,
        .local_input = local_input,
        .group_input = group_input,
        .texture_vec4 = texture_vec4,
        .texture_scalar = texture_scalar,
        .shared_words = shared_words,
        .shared_element_pointer = shared_element_pointer,
        .shared_array = shared_array,
        .push = push,
        .push_pointer = push_pointer,
        .local_words = local_words,
        .local_element_pointer = local_element_pointer,
        .local_array = local_array,
        .atomic_old_var = atomic_old_var,
        .stack_ptr = stack_ptr,
        .stack = stack,
        .call_ptr = call_ptr,
        .call_stack = call_stack,
        .pc = pc,
        .block_start = block_start,
        .structure = structure,
    };
    for (u32 i = 11; structure || i < n; i++) {
        /* structured: the blocks in the order their constructs need, the constructs written between them */
        while (structure && i >= se.body_end) {
            int step = shader_structure_step(c, &se);
            if (step == SE_BODY) {
                i = se.body_start;
                break;
            }
            if (step == SE_DONE) {
                i = n;
                break;
            }
            /* control runs off the program's end, which ends the dispatch loop: what follows it */
            if (c->discarded) {
                u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
                u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
                spv_emit_selection_merge(&c->emit, kept);
                spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
                spv_emit_label(&c->emit, kill_label);
                spv_emit_kill(&c->emit);
                spv_emit_label(&c->emit, kept);
            }
            if (passthrough)
                shader_geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs,
                                            point_size, layer_output, i32_type, clip_output, clip_count, primitive_id,
                                            primitive_id_out, depth_fix, push, push_pointer);
            if (xfb_on && stage != ST_GS) shader_xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
            if (depth_fix && stage != ST_GS) shader_vertex_depth_fix(c, position, push, push_pointer, vec4);
            spv_emit_return(&c->emit);
            se.open = 0;
        }
        if (i >= n) break;
        if (!IDX_OK(i)) continue;
        if (c->swapped) { /* the array element an indexed texture took for the instruction before */
            c->sampler[c->swapped - 1] = c->tex_array[c->swapped - 1];
            c->swapped = 0;
        }
        if (has_flow && !structure && block_start[i]) { spv_emit_label(&c->emit, case_labels[case_index++]); }
        if (has_flow && !structure &&
            (shader_next_instruction(i, n) >= n || block_start[shader_next_instruction(i, n)])) {
            u32 following = spv_emit_constant_u32(&c->emit, c->u32_type, shader_next_instruction(i, n));
            spv_emit_store(&c->emit, pc, following);
        }
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        u32 guard_merge = 0;
        c->guard = 0; /* the instruction before may have ended its translation early */
        if (structure && (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT)) {
            /* the branch that ends the block: the structure's SHS_IF takes its condition */
            c->branch_cond = 0;
            if (!flow_always(w)) {
                c->branch_cond = pred_read(c, B(w, 16, 3), B(w, 19, 1));
                if (B(w, 0, 5) != 15) c->branch_cond = cc_and(c, c->branch_cond, shader_cc_test(c, B(w, 0, 5)));
            }
            continue;
        }
        int skip_helper = stage == ST_FS && shader_helper_side_effect(op);
        if (op != O_SSY && op != O_PBK && op != O_PCNT && op != O_PEXIT && op != O_CAL &&
            (B(w, 16, 3) != 7 || B(w, 19, 1) || (shader_flow_has_cc(op) && B(w, 0, 5) != 15) || skip_helper)) {
            u32 condition = pred_read(c, B(w, 16, 3), B(w, 19, 1));
            if (shader_flow_has_cc(op) && B(w, 0, 5) != 15)
                condition = cc_and(c, condition, shader_cc_test(c, B(w, 0, 5)));
            if (skip_helper) condition = cc_and(c, condition, cc_not(c, shader_lane_dead(c)));
            if (shader_pure_op(op) && !skip_helper && !(shader_flow_has_cc(op) && B(w, 0, 5) != 15))
                c->guard = condition;
            else {
                u32 execute = spv_emit_id(&c->emit);
                guard_merge = spv_emit_id(&c->emit);
                spv_emit_selection_merge(&c->emit, guard_merge);
                spv_emit_branch_conditional(&c->emit, condition, execute, guard_merge);
                spv_emit_label(&c->emit, execute);
            }
        }
        /* the instruction to the family whose branches take its opcode */
        switch (op_family[op]) {
            case FAM_FLOW: emit_flow(&tr, i, w, op, d, a, b); break;
            case FAM_IO: emit_io(&tr, i, w, op, d, a, b); break;
            case FAM_THREAD: emit_thread(&tr, i, w, op, d, a, b); break;
            case FAM_INT: emit_int(&tr, i, w, op, d, a, b); break;
            case FAM_FLOAT: emit_float(&tr, i, w, op, d, a, b); break;
            case FAM_MEMORY: emit_memory(&tr, i, w, op, d, a, b); break;
            case FAM_TEXTURE: emit_texture(&tr, i, w, op, d, a, b); break;
        }
        /* no texture is sparse, so every texel a fetch reads is resident */
        pred_write(c, shader_sparse_predicate(w, op), c->false_value);
        c->guard = 0;
        if (guard_merge) {
            if (op != O_EXIT || (c->discarded && !structure)) spv_emit_branch(&c->emit, guard_merge);
            spv_emit_label(&c->emit, guard_merge);
        } else if (op == O_EXIT && !has_flow)
            break;
        u32 next = shader_next_instruction(i, n);
        if (has_flow && !structure && (next >= n || block_start[next]) &&
            (guard_merge || op != O_EXIT || has_pexit || c->discarded))
            spv_emit_branch(&c->emit, dispatch_merge);
    }
    if (structure && se.open) spv_emit_void_op(&c->emit, SPV_OP_UNREACHABLE, NULL, 0); /* every path ended before */
    if (has_flow && !structure) {
        /* A branch outside the decoded instruction range must terminate. A switch
           default that simply loops would leave a GPU invocation spinning forever. */
        if (converge) { /* lanes ahead of the subgroup's lowest pc wait for this iteration */
            spv_emit_label(&c->emit, wait_label);
            spv_emit_branch(&c->emit, dispatch_merge);
        }
        spv_emit_label(&c->emit, dispatch_default);
        spv_emit_store(&c->emit, pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch(&c->emit, dispatch_merge);
        spv_emit_label(&c->emit, dispatch_merge);
        u32 next_pc = spv_emit_load(&c->emit, c->u32_type, pc);
        u32 finished =
            spv_emit_i_equal(&c->emit, c->bool_type, next_pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch_conditional(&c->emit, finished, loop_merge, loop_continue);
        spv_emit_label(&c->emit, loop_continue);
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_merge);
        if (c->discarded) {
            u32 kill_label = spv_emit_id(&c->emit), kept = spv_emit_id(&c->emit);
            u32 discard = spv_emit_load(&c->emit, c->bool_type, c->discarded);
            spv_emit_selection_merge(&c->emit, kept);
            spv_emit_branch_conditional(&c->emit, discard, kill_label, kept);
            spv_emit_label(&c->emit, kill_label);
            spv_emit_kill(&c->emit);
            spv_emit_label(&c->emit, kept);
        }
        if (passthrough)
            shader_geometry_passthrough(c, si, in_vertices, vec4, position, position_in, output_attrs, inputs,
                                        point_size, layer_output, i32_type, clip_output, clip_count, primitive_id,
                                        primitive_id_out, depth_fix, push, push_pointer);
        if (xfb_on && stage != ST_GS) shader_xfb_store(c, vec4, output_attrs, attr_private, xfb_components);
        if (depth_fix && stage != ST_GS) shader_vertex_depth_fix(c, position, push, push_pointer, vec4);
        spv_emit_return(&c->emit);
    }
    spv_emit_function_end(&c->emit);
    u32* result = spv_emit_finish(&c->emit, word_count);
    spv_emit_destroy(&c->emit);
    free(c->ftz);
    free(c->ftz_of);
    free(c->ftz_of_block);
    free(c);
    free(case_labels); /* the literals and block starts too */
    free(structure);
    free(se.label); /* and what was used */
    free(pop_targets);
    free(gb_at);
    ShaderInfo next = {0};
    next.stage = stage;
    memcpy(next.cs_local, si->cs_local, sizeof next.cs_local);
    next.shared_size = si->shared_size;
    next.attr_int_mask = si->attr_int_mask;
    next.attr_sint_mask = si->attr_sint_mask;
    next.clip_enable = si->clip_enable;
    memcpy(next.gp_skip_mask, si->gp_skip_mask, sizeof next.gp_skip_mask);
    next.uses_clip_mask = clip_used;
    next.writes_viewport = viewport_used;
    next.rt_int_mask = si->rt_int_mask;
    next.rt_sint_mask = si->rt_sint_mask;
    next.tex_int_mask = si->tex_int_mask;
    next.tex_sint_mask = si->tex_sint_mask;
    next.tex_dc_color_mask = si->tex_dc_color_mask;
    next.tex_dc_color_funcs = si->tex_dc_color_funcs;
    next.in_unfed_mask = si->in_unfed_mask;
    next.ntex = texture_count;
    next.tex_own_pixel_mask = stage == ST_FS ? tex_read & ~tex_elsewhere : 0;
    next.tex_query_mask = tex_query;
    memcpy(next.tex_query_hint, si->tex_query_hint, sizeof next.tex_query_hint);
    next.nimg = image_info.nimg;
    memcpy(next.img, image_info.img, (size_t)image_info.nimg * sizeof image_info.img[0]);
    next.ngbuf = ngbuf;
    next.gbuf_wmask = gbuf_wmask;
    memcpy(next.gbuf_cbuf_off, gbuf_off, (size_t)ngbuf * sizeof gbuf_off[0]);
    next.fp32_flush = fp32_flush;
    next.fp32_preserve = fp32_preserve;
    next.uses_position_in = needs_fragcoord;
    memcpy(next.tex, textures, (size_t)texture_count * sizeof textures[0]);
    for (u32 i = 0; i < 18; i++)
        if (cbuf_used[i]) {
            next.cbuf_mask |= 1u << i;
            next.cbuf_max[i] = cbuf_max[i];
        }
    for (u32 i = 0; i < 32; i++) {
        if (inputs_used[i]) next.in_mask |= 1u << i;
        if (outputs_used[i]) next.out_mask |= 1u << i;
    }
    if (indexed_store) next.out_mask = ~0u;
    memcpy(next.sph, sph, sizeof sph);
    *si = next;
    return result;
}
u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup) {
    u64* copy;
    const u64* insn;
    u32* r;
    if (!code) return NULL;
    insn = shader_words(code, max_bytes, &copy);
    if (!insn) return NULL;
    u64* moved = NULL;
    u32 legacy_fs = 0, len = 0, sph[20];
    if (stage >= ST_VS && stage <= ST_FS && max_bytes >= 96) {
        shader_hash(code, max_bytes, &len);
        for (u32 i = 0; i < 20; i++) sph[i] = snx_ld_le32(code + i * 4);
        if (shader_legacy_varyings(insn, len / 8, max_bytes / 8, sph, stage, si, &moved, &legacy_fs)) {
            free(copy);
            if (!si->unsupported) return direct_reject(si, "legacy varyings: out of memory");
            return NULL;
        }
    }
    r = to_spirv(code, moved ? moved : insn, max_bytes, stage, si, word_count, can_flush, can_preserve,
                 can_preserve_f64, can_subgroup, legacy_fs);
    free(moved);
    free(copy);
    return r;
}