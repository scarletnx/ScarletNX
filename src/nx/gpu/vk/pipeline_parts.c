/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries: the choice and the dynamic pre-rasterization state (pipeline_parts.h) */
#include "nx/gpu/vk/pipeline_parts.h"
#include <stdio.h>
#include <string.h>

void vk_gpl_bans(VkGplDevice* d, VkBanSay say) {
#define EDS3 "VK_EXT_extended_dynamic_state3.extendedDynamicState3"
    const struct {
        const char* key;
        int* member;
    } members[] = {
        {EDS3 "PolygonMode", &d->polygon},
        {EDS3 "DepthClampEnable", &d->depth_clamp},
        {EDS3 "ProvokingVertexMode", &d->provoking},
        {EDS3 "ConservativeRasterizationMode", &d->conservative_mode},
        {EDS3 "ExtraPrimitiveOverestimationSize", &d->conservative_size},
        {"VK_EXT_extended_dynamic_state2.extendedDynamicState2PatchControlPoints", &d->patch},
    };
    for (size_t i = 0; i < sizeof members / sizeof *members; i++)
        if (vk_ban(members[i].key, *members[i].member, say)) *members[i].member = 0;
    d->depth_range_ban = vk_ban(EDS3 "DepthClipNegativeOneToOne", d->depth_range, say);
    if (d->depth_range_ban) d->depth_range = 0;
#undef EDS3
}

void vk_depth_range_line(const VkGplDevice* d, u32 dyn_pr, char* line, size_t n) {
    if (dyn_pr & VK_DYN_PR_DEPTH_RANGE) {
        snprintf(line, n,
                 "vulkan: a depth range of -1 to 1 is the device's (VK_EXT_depth_clip_control, as dynamic state)");
        return;
    }
    char why[160];
    if (!d->depth_clip_control)
        snprintf(why, sizeof why, "the device has no VK_EXT_depth_clip_control");
    else if (d->depth_range_ban)
        snprintf(why, sizeof why,
                 "extension_bans.txt:%u holds for extendedDynamicState3DepthClipNegativeOneToOne on this driver",
                 (unsigned)d->depth_range_ban);
    else if (!d->depth_range)
        snprintf(why, sizeof why, "the device has no extendedDynamicState3DepthClipNegativeOneToOne");
    else
        snprintf(why, sizeof why, "graphics pipeline libraries are off, and their dynamic state with them");
    snprintf(line, n,
             "vulkan: a depth range of -1 to 1 is made by the last stage before rasterization, z = (z + w) / 2 (%s)",
             why);
}

int vk_gpl_choose(const VkGplDevice* d, u32* dyn_pr, char* line, size_t n) {
    /* a link without fast linking compiles, so it would save nothing; without independent interpolation the
       pre-rasterization and fragment parts must agree on decorations the translator doesn't promise, and
       vkd3d-proton leaves the extension off without it too */
    const char* off = !d->setting                     ? "pipeline_libraries=0"
                      : !d->has_ext                   ? "the device has no VK_EXT_graphics_pipeline_library"
                      : !d->library                   ? "the device has VK_EXT_graphics_pipeline_library without "
                                                        "graphicsPipelineLibrary"
                      : !d->fast_link                 ? "the device links pipeline libraries without fast linking"
                      : !d->independent_interpolation ? "the device has no independent interpolation decorations for "
                                                        "pipeline libraries"
                                                      : NULL;
    *dyn_pr = 0;
    if (off) {
        snprintf(line, n, "vulkan: graphics pipeline libraries off (%s): each pipeline compiles its shaders whole", off);
        return 0;
    }
    if (d->polygon) *dyn_pr |= VK_DYN_PR_POLYGON;
    if (d->depth_clamp) *dyn_pr |= VK_DYN_PR_DEPTH_CLAMP;
    if (d->provoking && d->has_provoking_last) *dyn_pr |= VK_DYN_PR_PROVOKING;
    if (d->conservative_mode && d->conservative_size && d->has_conservative) *dyn_pr |= VK_DYN_PR_CONSERVATIVE;
    if (d->patch) *dyn_pr |= VK_DYN_PR_PATCH;
    if (d->depth_range && d->depth_clip_control) *dyn_pr |= VK_DYN_PR_DEPTH_RANGE;
    static const char* const names[6] = {"polygon mode",         "depth clamp",
                                         "provoking vertex",     "conservative rasterization",
                                         "patch control points", "depth range"};
    char dyn[160];
    size_t at = 0;
    dyn[0] = 0;
    for (int i = 0; i < 6; i++)
        if ((*dyn_pr >> i & 1) && at < sizeof dyn)
            at += (size_t)snprintf(dyn + at, sizeof dyn - at, "%s%s", at ? ", " : "", names[i]);
    snprintf(line, n,
             "vulkan: graphics pipeline libraries on: a new pipeline links the parts of its shaders, each compiled "
             "once, and is optimized in the background; dynamic pre-rasterization state: %s",
             at ? dyn : "none");
    return 1;
}

void vk_pipe_part_src(u32 part, const void* key, PipeKey* out) {
    memset(out, 0, sizeof *out); /* PipeKey's bytes are compared and hashed */
    if (part == VK_PART_VI) {
        const PipeViKey* vi = key;
        out->topo_class = vi->topo_class;
        out->vi = vi->vi;
    } else if (part == VK_PART_PR) {
        const PipePrKey* pr = key;
        out->vs = pr->vs;
        out->tcs = pr->tcs;
        out->tes = pr->tes;
        out->gs = pr->gs;
        out->arrays = pr->arrays;
        out->patch = pr->patch;
        out->polygon = pr->polygon;
        out->depth_clamp = pr->depth_clamp;
        out->provoking_last = pr->provoking_last;
        out->conservative = pr->conservative;
    } else if (part == VK_PART_FS) {
        const PipeFsKey* f = key;
        out->fs = f->fs;
        out->arrays = f->arrays;
        out->fs_spec = f->spec;
        out->feedback = f->feedback;
        out->sample_shading = f->sample_shading;
        out->samples = f->samples;
        out->sample_off = f->sample_off;
        out->alpha_to_coverage = f->alpha_to_coverage;
        out->alpha_to_one = f->alpha_to_one;
        out->coverage = f->coverage;
    } else {
        const PipeFoKey* fo = key;
        out->ncolor = fo->ncolor;
        for (u32 i = 0; i < 8; i++) {
            out->color[i] = fo->color[i];
            out->blend[i].en = fo->blend[i].en;
            out->blend[i].sc = fo->blend[i].sc;
            out->blend[i].dc = fo->blend[i].dc;
            out->blend[i].cop = fo->blend[i].cop;
            out->blend[i].sa = fo->blend[i].sa;
            out->blend[i].da = fo->blend[i].da;
            out->blend[i].aop = fo->blend[i].aop;
            out->blend[i].mask = fo->blend[i].mask;
        }
        out->depth = fo->depth;
        out->stencil = fo->stencil;
        out->logic_op = fo->logic_op;
        out->samples = fo->samples;
        out->sample_off = fo->sample_off;
        out->alpha_to_coverage = fo->alpha_to_coverage;
        out->alpha_to_one = fo->alpha_to_one;
        out->sample_shading = fo->sample_shading;
        out->feedback = fo->feedback;
        out->coverage = fo->coverage;
        out->advanced = fo->advanced;
        /* the masks hold already whether there is a fragment program: any one passes them through */
        out->fs = (VkShaderModule)(uintptr_t)1;
    }
}

/* every dynamic state a draw pipeline can have, in the order pipe_create always listed them (a pipeline cache entry
   depends on it), with the part that holds it (the specification's "Graphics Pipeline Library" says which state each
   part holds) and when it is there */
enum { DYN_ALWAYS, DYN_FEEDBACK, DYN_STRIDE, DYN_VERTEX_INPUT, DYN_PR, DYN_PR_TESS };
static const struct {
    VkDynamicState state;
    u32 part; /* VK_PART_* */
    u32 when; /* DYN_*: for DYN_PR and DYN_PR_TESS also the VK_DYN_PR_* bit, from bit 8 */
} g_dyn_states[] = {
    {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_CULL_MODE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_FRONT_FACE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_PART_VI, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_OP, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_PART_VI, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_LINE_WIDTH, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BIAS, VK_PART_PR, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_PART_FO, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_PART_FS, DYN_ALWAYS},
    {VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT, VK_PART_FS | VK_PART_FO, DYN_FEEDBACK},
    {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_PART_VI, DYN_STRIDE},
    {VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_PART_VI, DYN_VERTEX_INPUT},
    {VK_DYNAMIC_STATE_POLYGON_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_POLYGON << 8},
    {VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_DEPTH_CLAMP << 8},
    {VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_PROVOKING << 8},
    {VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_CONSERVATIVE << 8},
    {VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_CONSERVATIVE << 8},
    {VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT, VK_PART_PR, DYN_PR_TESS | VK_DYN_PR_PATCH << 8},
    {VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT, VK_PART_PR, DYN_PR | VK_DYN_PR_DEPTH_RANGE << 8},
};
typedef char dyn_states_fit[sizeof g_dyn_states / sizeof *g_dyn_states <= VK_PIPE_DYN_MAX ? 1 : -1];

u32 vk_pipe_dynamic(u32 parts, const PipeDynIn* in, VkDynamicState* out) {
    u32 n = 0;
    for (u32 i = 0; i < sizeof g_dyn_states / sizeof *g_dyn_states; i++) {
        u32 when = g_dyn_states[i].when & 0xFF, bit = g_dyn_states[i].when >> 8;
        int on = when == DYN_ALWAYS         ? 1
                 : when == DYN_FEEDBACK     ? in->feedback_dynamic
                 : when == DYN_STRIDE       ? in->stride
                 : when == DYN_VERTEX_INPUT ? in->vertex_input
                 : when == DYN_PR           ? (in->dyn_pr & bit) != 0
                                            : (in->dyn_pr & bit) && in->tess;
        if (in->mesh && g_dyn_states[i].part == VK_PART_VI) on = 0; /* a mesh pipeline has no vertex input state */
        if (on && (g_dyn_states[i].part & parts)) out[n++] = g_dyn_states[i].state;
    }
    return n;
}

u32 vk_pipe_fs_spec(u32 declared, int alpha_test, u32 alpha_func, int lower_left, int sprite_origin) {
    u32 spec = 0;
    if ((declared >> SH_SPEC_ALPHA & 1) && alpha_test) spec |= SH_SPEC_ALPHA_ALWAYS ^ (alpha_func & PIPE_SPEC_ALPHA);
    if ((declared >> SH_SPEC_LOWER_LEFT & 1) && lower_left) spec |= PIPE_SPEC_LOWER_LEFT;
    if ((declared >> SH_SPEC_SPRITE_FLIP & 1) && !sprite_origin == !lower_left) spec |= PIPE_SPEC_SPRITE_FLIP;
    return spec;
}
void vk_pipe_fs_spec_values(u32 fs_spec, u32 values[SH_SPEC_COUNT]) {
    values[SH_SPEC_ALPHA] = SH_SPEC_ALPHA_ALWAYS ^ (fs_spec & PIPE_SPEC_ALPHA);
    values[SH_SPEC_LOWER_LEFT] = (fs_spec & PIPE_SPEC_LOWER_LEFT) != 0;
    values[SH_SPEC_SPRITE_FLIP] = (fs_spec & PIPE_SPEC_SPRITE_FLIP) != 0;
}

void vk_pipe_split(const PipeKey* k, VkShaderModule fs, const PipeSplitDevice* d, PipeViKey* vi, PipePrKey* pr,
                   PipeFsKey* f, PipeFoKey* fo) {
    *vi = (PipeViKey){0};
    vi->topo_class = k->topo_class;
    vi->vi = k->vi;

    *pr = (PipePrKey){0};
    pr->vs = k->vs;
    pr->tcs = k->tcs;
    pr->tes = k->tes;
    pr->gs = k->gs;
    pr->arrays = k->arrays & VK_ARRAYS_PR;
    if (!(d->dyn_pr & VK_DYN_PR_PATCH)) pr->patch = k->patch;
    if (!(d->dyn_pr & VK_DYN_PR_POLYGON)) pr->polygon = k->polygon;
    if (!(d->dyn_pr & VK_DYN_PR_DEPTH_CLAMP)) pr->depth_clamp = k->depth_clamp;
    if (!(d->dyn_pr & VK_DYN_PR_PROVOKING)) pr->provoking_last = k->provoking_last;
    if (!(d->dyn_pr & VK_DYN_PR_CONSERVATIVE)) pr->conservative = k->conservative;

    *f = (PipeFsKey){0};
    f->fs = fs;
    f->arrays = k->arrays & VK_ARRAYS_FS;
    f->spec = k->fs_spec;
    if (!d->feedback_dynamic) f->feedback = k->feedback;
    if (k->sample_shading) {
        f->sample_shading = k->sample_shading;
        f->samples = k->samples;
        f->sample_off = k->sample_off;
        f->alpha_to_coverage = k->alpha_to_coverage;
        f->alpha_to_one = k->alpha_to_one;
        f->coverage = k->coverage;
    }

    *fo = (PipeFoKey){0};
    fo->ncolor = k->ncolor;
    for (u32 i = 0; i < 8; i++) fo->color[i] = k->color[i];
    fo->depth = k->depth;
    fo->stencil = k->stencil;
    for (u32 i = 0; i < 8; i++) {
        fo->blend[i].en = k->blend[i].en;
        fo->blend[i].sc = k->blend[i].sc;
        fo->blend[i].dc = k->blend[i].dc;
        fo->blend[i].cop = k->blend[i].cop;
        fo->blend[i].sa = k->blend[i].sa;
        fo->blend[i].da = k->blend[i].da;
        fo->blend[i].aop = k->blend[i].aop;
        fo->blend[i].mask = k->fs ? k->blend[i].mask : 0;
    }
    fo->logic_op = k->logic_op;
    fo->samples = k->samples;
    fo->sample_off = k->sample_off;
    fo->alpha_to_coverage = k->alpha_to_coverage;
    fo->alpha_to_one = k->alpha_to_one;
    fo->sample_shading = k->sample_shading;
    if (!d->feedback_dynamic) fo->feedback = k->feedback;
    fo->coverage = k->coverage;
    fo->advanced = k->advanced;
}

