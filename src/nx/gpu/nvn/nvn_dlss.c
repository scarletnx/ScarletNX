/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's DLSS of the Ounce (API 231): the network builder and the parameters of an evaluation. Both are the
   game's objects, which the runtime keeps at the offsets of the game's driver, as its setters and getters do */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

#define NB(b) NVN_OBJ(NvnDlssNetworkBuilder, b)
#define DP(p) NVN_OBJ(NvnDlssParameters, p)

/* ============================================================ network builder */
/* the driver keeps the device, clears the rest and takes one evaluation entry */
void nvn_DlssNetworkBuilderSetDefaults(CPU* c, u64 b) {
    NvnDlssNetworkBuilder* o = NB(b);
    (void)c;
    u64 d = o->device;
    *o = (NvnDlssNetworkBuilder){0};
    o->device = d;
    o->max_entries = 1;
}
void nvn_DlssNetworkBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    NB(b)->device = d;
}
u64 nvn_DlssNetworkBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    return NB(b)->device;
}
void nvn_DlssNetworkBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    NB(b)->flags = flags;
}
i32 nvn_DlssNetworkBuilderGetFlags(CPU* c, u64 b) {
    (void)c;
    return NB(b)->flags;
}
void nvn_DlssNetworkBuilderSetMethod(CPU* c, u64 b, i32 method) {
    (void)c;
    NB(b)->method = method;
}
i32 nvn_DlssNetworkBuilderGetMethod(CPU* c, u64 b) {
    (void)c;
    return NB(b)->method;
}
void nvn_DlssNetworkBuilderSetPreset(CPU* c, u64 b, i32 preset) {
    (void)c;
    NB(b)->preset = preset;
}
i32 nvn_DlssNetworkBuilderGetPreset(CPU* c, u64 b) {
    (void)c;
    return NB(b)->preset;
}
void nvn_DlssNetworkBuilderSetOutputResolution(CPU* c, u64 b, i32 width, i32 height) {
    (void)c;
    NB(b)->width = width;
    NB(b)->height = height;
}
void nvn_DlssNetworkBuilderGetOutputResolution(CPU* c, u64 b, u64 width, u64 height) {
    (void)c;
    gw32(width, (u32)NB(b)->width);
    gw32(height, (u32)NB(b)->height);
}
void nvn_DlssNetworkBuilderSetMaxEvaluationEntries(CPU* c, u64 b, i32 n) {
    (void)c;
    NB(b)->max_entries = n;
}
i32 nvn_DlssNetworkBuilderGetMaxEvaluationEntries(CPU* c, u64 b) {
    (void)c;
    return NB(b)->max_entries;
}
void nvn_DlssNetworkBuilderSetStorage(CPU* c, u64 b, u64 memory, u64 size) {
    (void)c;
    NB(b)->memory = memory;
    NB(b)->memory_size = size;
}
u64 nvn_DlssNetworkBuilderGetMemory(CPU* c, u64 b) {
    (void)c;
    return NB(b)->memory;
}
u64 nvn_DlssNetworkBuilderGetMemorySize(CPU* c, u64 b) {
    (void)c;
    return NB(b)->memory_size;
}
/* the pool and the 128 bytes of texture IDs the network's own textures take */
void nvn_DlssNetworkBuilderSetTextureIDs(CPU* c, u64 b, u64 pool, u64 ids) {
    (void)c;
    memcpy(NB(b)->ids, GP(ids), sizeof NB(b)->ids);
    NB(b)->pool = pool;
}

/* ============================================================ parameters */
/* the driver's defaults: all zero but the motion vector scale (1, 1), the pre-exposure (1) and the four overlays
   (-1) */
void nvn_DlssParametersSetDefaults(CPU* c, u64 p) {
    NvnDlssParameters* o = DP(p);
    (void)c;
    *o = (NvnDlssParameters){0};
    o->motion_scale[0] = o->motion_scale[1] = 1.0f;
    o->pre_exposure = 1.0f;
    for (u32 i = 0; i < 4; i++) o->overlay[i] = -1;
}
static void pair(NvnTexturePair* to, u64 t, u64 v) { *to = (NvnTexturePair){t, v}; }
void nvn_DlssParametersSetInputColor(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->input, t, v);
}
void nvn_DlssParametersSetOutputColor(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->output, t, v);
}
void nvn_DlssParametersSetMotionVectors(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->motion, t, v);
}
void nvn_DlssParametersSetDepth(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->depth, t, v);
}
void nvn_DlssParametersSetBiasCurrentColorMask(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->bias, t, v);
}
void nvn_DlssParametersSetExposureTexture(CPU* c, u64 p, u64 t, u64 v) {
    (void)c;
    pair(&DP(p)->exposure, t, v);
}
void nvn_DlssParametersSetJitterOffset(CPU* c, u64 p, f32 x, f32 y) {
    (void)c;
    DP(p)->jitter[0] = x;
    DP(p)->jitter[1] = y;
}
void nvn_DlssParametersGetJitterOffset(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    memcpy(GP(x), &DP(p)->jitter[0], 4);
    memcpy(GP(y), &DP(p)->jitter[1], 4);
}
void nvn_DlssParametersSetMotionVectorScale(CPU* c, u64 p, f32 x, f32 y) {
    (void)c;
    DP(p)->motion_scale[0] = x;
    DP(p)->motion_scale[1] = y;
}
void nvn_DlssParametersGetMotionVectorScale(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    memcpy(GP(x), &DP(p)->motion_scale[0], 4);
    memcpy(GP(y), &DP(p)->motion_scale[1], 4);
}
void nvn_DlssParametersSetPreExposure(CPU* c, u64 p, f32 v) {
    (void)c;
    DP(p)->pre_exposure = v;
}
f32 nvn_DlssParametersGetPreExposure(CPU* c, u64 p) {
    (void)c;
    return DP(p)->pre_exposure;
}
void nvn_DlssParametersSetHistoryReset(CPU* c, u64 p, int reset) {
    (void)c;
    DP(p)->history_reset = (u8)(reset != 0);
}
int nvn_DlssParametersGetHistoryReset(CPU* c, u64 p) {
    (void)c;
    return DP(p)->history_reset;
}
void nvn_DlssParametersSetEvaluationIndex(CPU* c, u64 p, i32 index) {
    (void)c;
    DP(p)->evaluation_index = index;
}
i32 nvn_DlssParametersGetEvaluationIndex(CPU* c, u64 p) {
    (void)c;
    return DP(p)->evaluation_index;
}
static void set2(i32* to, i32 x, i32 y) {
    to[0] = x;
    to[1] = y;
}
static void get2(const i32* from, u64 x, u64 y) {
    gw32(x, (u32)from[0]);
    gw32(y, (u32)from[1]);
}
void nvn_DlssParametersSetInputSubrectangleSize(CPU* c, u64 p, i32 width, i32 height) {
    (void)c;
    set2(DP(p)->input_size, width, height);
}
void nvn_DlssParametersGetInputSubrectangleSize(CPU* c, u64 p, u64 width, u64 height) {
    (void)c;
    get2(DP(p)->input_size, width, height);
}
/* the subrectangles' bases, in the order of NvnDlssParameters.base */
enum { BASE_INPUT, BASE_DEPTH, BASE_MOTION, BASE_BIAS, BASE_OUTPUT };
void nvn_DlssParametersSetInputColorSubrectangleBase(CPU* c, u64 p, i32 x, i32 y) {
    (void)c;
    set2(DP(p)->base[BASE_INPUT], x, y);
}
void nvn_DlssParametersGetInputColorSubrectangleBase(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    get2(DP(p)->base[BASE_INPUT], x, y);
}
void nvn_DlssParametersSetDepthSubrectangleBase(CPU* c, u64 p, i32 x, i32 y) {
    (void)c;
    set2(DP(p)->base[BASE_DEPTH], x, y);
}
void nvn_DlssParametersGetDepthSubrectangleBase(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    get2(DP(p)->base[BASE_DEPTH], x, y);
}
void nvn_DlssParametersSetMotionVectorSubrectangleBase(CPU* c, u64 p, i32 x, i32 y) {
    (void)c;
    set2(DP(p)->base[BASE_MOTION], x, y);
}
void nvn_DlssParametersGetMotionVectorSubrectangleBase(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    get2(DP(p)->base[BASE_MOTION], x, y);
}
void nvn_DlssParametersSetBiasCurrentColorSubrectangleBase(CPU* c, u64 p, i32 x, i32 y) {
    (void)c;
    set2(DP(p)->base[BASE_BIAS], x, y);
}
void nvn_DlssParametersGetBiasCurrentColorSubrectangleBase(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    get2(DP(p)->base[BASE_BIAS], x, y);
}
void nvn_DlssParametersSetOutputSubrectangleBase(CPU* c, u64 p, i32 x, i32 y) {
    (void)c;
    set2(DP(p)->base[BASE_OUTPUT], x, y);
}
void nvn_DlssParametersGetOutputSubrectangleBase(CPU* c, u64 p, u64 x, u64 y) {
    (void)c;
    get2(DP(p)->base[BASE_OUTPUT], x, y);
}
/* the debug overlays, in the order of NvnDlssParameters.overlay */
void nvn_DlssParametersSetOverlayDebugLayer(CPU* c, u64 p, i32 on) {
    (void)c;
    DP(p)->overlay[0] = on;
}
void nvn_DlssParametersSetOverlayFullScreen(CPU* c, u64 p, i32 on) {
    (void)c;
    DP(p)->overlay[1] = on;
}
void nvn_DlssParametersSetOverlayShowNans(CPU* c, u64 p, i32 on) {
    (void)c;
    DP(p)->overlay[2] = on;
}
void nvn_DlssParametersSetOverlayJitterDebug(CPU* c, u64 p, i32 on) {
    (void)c;
    DP(p)->overlay[3] = on;
}
/* the pool SetTextureIDs gave, and its 128 bytes of IDs, as the driver's getters read them back */
u64 nvn_DlssNetworkBuilderGetTexturePool(CPU* c, u64 b) {
    (void)c;
    return NB(b)->pool;
}
void nvn_DlssNetworkBuilderGetTextureIDs(CPU* c, u64 b, u64 ids) {
    (void)c;
    memcpy(GP(ids), NB(b)->ids, sizeof NB(b)->ids);
}

/* ============================================================ network: a stand-in for the upscaler */
/* No network runs: an evaluation goes to the renderer's upscaler (FSR 2, the DLSS upscaler setting), or is a filtered
   copy of the input color's subrectangle onto the output's (nvn_cmd.c's nvn_CommandBufferEvaluateDlss, nvn_copy.c's
   OP_DLSS). The render sizes the driver answers come from a backend this runtime
   doesn't have, so they are taken from the method as NGX's quality modes scale the output: the driver's methods are
   assumed to number them as NGX does (0 performance, 1 balanced, 2 quality, 3 ultra performance, 4 ultra quality,
   5 DLAA), any other is performance's half. The largest render size is the output's, the smallest a third of it */
static void render_sizes(i32 width, i32 height, i32 method, i32 out[3][2]) {
    static const i32 scale[6][2] = {{1, 2}, {58, 100}, {2, 3}, {1, 3}, {77, 100}, {1, 1}};
    /* the DLSS render size setting: FSR 2's own quality modes in place of the game's (1/1.5, 1/1.7, 1/2, 1/3) */
    static const i32 set[6][2] = {{0, 0}, {1, 1}, {2, 3}, {10, 17}, {1, 2}, {1, 3}};
    const i32* s = method >= 0 && method < 6 ? scale[method] : scale[0];
    if (g_settings.dlss_render_size > 0 && g_settings.dlss_render_size < 6) s = set[g_settings.dlss_render_size];
    out[0][0] = (i32)(((i64)width * s[0] + s[1] / 2) / s[1]);
    out[0][1] = (i32)(((i64)height * s[0] + s[1] / 2) / s[1]);
    out[1][0] = width;
    out[1][1] = height;
    out[2][0] = (width + 2) / 3;
    out[2][1] = (height + 2) / 3;
    for (u32 i = 0; i < 3; i++) {
        if (out[i][0] < 1) out[i][0] = 1;
        if (out[i][1] < 1) out[i][1] = 1;
    }
}
int nvn_DlssNetworkBuilderGetOptimalSettings(CPU* c, u64 b, u64 width, u64 height, u64 max_width, u64 max_height,
                                             u64 min_width, u64 min_height) {
    i32 r[3][2];
    (void)c;
    render_sizes(NB(b)->width, NB(b)->height, NB(b)->method, r);
    if (width) gw32(width, (u32)r[0][0]);
    if (height) gw32(height, (u32)r[0][1]);
    if (max_width) gw32(max_width, (u32)r[1][0]);
    if (max_height) gw32(max_height, (u32)r[1][1]);
    if (min_width) gw32(min_width, (u32)r[2][0]);
    if (min_height) gw32(min_height, (u32)r[2][1]);
    return 1;
}
/* the storage the game gives the network (SetStorage): no network uses it, so a small amount, page aligned, with the
   64 KiB the driver adds for each evaluation entry */
u64 nvn_DlssNetworkBuilderGetStorageSize(CPU* c, u64 b) {
    i32 n = NB(b)->max_entries;
    (void)c;
    return (1ull << 20) + ((u64)(n > 0 ? n : 1) << 16);
}
#define NW(n) NVN_OBJ(NvnDlssNetwork, n)
static void network_sizes(NvnDlssNetwork* o) {
    i32 r[3][2];
    render_sizes(o->output[0], o->output[1], o->method, r);
    memcpy(o->render, r[0], sizeof o->render);
    memcpy(o->render_max, r[1], sizeof o->render_max);
    memcpy(o->render_min, r[2], sizeof o->render_min);
}
/* the driver's Initialize: the network cleared, the builder's device, method, preset, flags and output size (the
   largest Resize may ask for), and the render sizes */
int nvn_DlssNetworkInitialize(CPU* c, u64 n, u64 b) {
    const NvnDlssNetworkBuilder* s = NB(b);
    NvnDlssNetwork* o = NW(n);
    (void)c;
    memset(o, 0, sizeof *o);
    o->device = s->device;
    o->method = o->method2 = s->method;
    o->preset = s->preset;
    o->builder_flags = s->flags;
    o->flags = s->flags & 0x4e;
    o->output[0] = o->output_max[0] = o->size[0] = s->width;
    o->output[1] = o->output_max[1] = o->size[1] = s->height;
    network_sizes(o);
    LOG("nvn: DLSS network %llx: output %dx%d, method %d, preset %d, flags %#x; no network runs, an evaluation "
        "is FSR 2's or a filtered copy (render size %dx%d, from %dx%d to %dx%d)\n",
        (unsigned long long)n, s->width, s->height, s->method, s->preset, (u32)s->flags, o->render[0], o->render[1],
        o->render_min[0], o->render_min[1], o->render_max[0], o->render_max[1]);
    return 1;
}
void nvn_DlssNetworkFinalize(CPU* c, u64 n) {
    (void)c;
    memset(NW(n), 0, sizeof(NvnDlssNetwork));
}
/* a new output size, no larger than the builder's (the driver answers false and changes nothing then) */
int nvn_DlssNetworkResize(CPU* c, u64 n, i32 width, i32 height) {
    NvnDlssNetwork* o = NW(n);
    (void)c;
    if (width > o->output_max[0] || height > o->output_max[1]) return 0;
    o->output[0] = o->size[0] = width;
    o->output[1] = o->size[1] = height;
    network_sizes(o);
    LOG_LIMIT(8, "nvn: DLSS network %llx resized to %dx%d (render size %dx%d)\n", (unsigned long long)n, width, height,
              o->render[0], o->render[1]);
    return 1;
}
