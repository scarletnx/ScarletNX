/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's DLSS of the Ounce (API 231): the network builder and the parameters of an evaluation. Both are the
   game's objects, which the runtime keeps at the offsets of the game's driver, as its setters and getters do */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"
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
