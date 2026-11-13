/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* fsr2.h over AMD FidelityFX Super Resolution 2.2.1's own interface (ffx_fsr2.h, vk/ffx_fsr2_vk.h; MIT, Copyright (c)
   2022-2023 Advanced Micro Devices, Inc.). C++ only because vk/ffx_fsr2_vk.h is (default arguments) */
#include "nx/gpu/vk/fsr2/fsr2.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"

struct SnxFsr2 {
    FfxFsr2Context context;
    void* scratch;
    SnxFsr2Info info;
};

static int g_snx_fsr2_error;
int snx_fsr2_error(void) { return g_snx_fsr2_error; }

int snx_fsr2_format(VkFormat format) {
    /* the backend's own table, which it keeps to itself: a resource of the format, described (no context is read) */
    FfxResource r = ffxGetTextureResourceVK(NULL, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, 1, format, NULL,
                                            FFX_RESOURCE_STATE_COMPUTE_READ);
    return r.description.format != FFX_SURFACE_FORMAT_UNKNOWN;
}

SnxFsr2* snx_fsr2_create(const SnxFsr2Info* info) {
    SnxFsr2* f = (SnxFsr2*)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->info = *info;
    size_t size = ffxFsr2GetScratchMemorySizeVK(info->physical);
    f->scratch = calloc(1, size);
    FfxFsr2ContextDescription d;
    memset(&d, 0, sizeof d);
    if (!f->scratch || (g_snx_fsr2_error = (int)ffxFsr2GetInterfaceVK(&d.callbacks, f->scratch, size, info->physical,
                                                                      vkGetDeviceProcAddr)) != FFX_OK) {
        free(f->scratch);
        free(f);
        return NULL;
    }
    d.device = ffxGetDeviceVK(info->device);
    d.maxRenderSize.width = info->render_max[0];
    d.maxRenderSize.height = info->render_max[1];
    d.displaySize.width = info->output[0];
    d.displaySize.height = info->output[1];
    d.flags = (info->flags & SNX_FSR2_HDR ? FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
              (info->flags & SNX_FSR2_DISPLAY_MOTION ? FFX_FSR2_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0) |
              (info->flags & SNX_FSR2_JITTERED_MOTION ? FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
              (info->flags & SNX_FSR2_INVERTED_DEPTH ? FFX_FSR2_ENABLE_DEPTH_INVERTED : 0) |
              (info->flags & SNX_FSR2_INFINITE_DEPTH ? FFX_FSR2_ENABLE_DEPTH_INFINITE : 0) |
              (info->flags & SNX_FSR2_AUTO_EXPOSURE ? FFX_FSR2_ENABLE_AUTO_EXPOSURE : 0) |
              FFX_FSR2_ENABLE_DYNAMIC_RESOLUTION; /* a dispatch's render size may be any up to render_max */
    if ((g_snx_fsr2_error = (int)ffxFsr2ContextCreate(&f->context, &d)) != FFX_OK) {
        free(f->scratch);
        free(f);
        return NULL;
    }
    return f;
}

void snx_fsr2_destroy(SnxFsr2* f) {
    if (!f) return;
    ffxFsr2ContextDestroy(&f->context);
    free(f->scratch);
    free(f);
}

static FfxResource image(SnxFsr2* f, const SnxFsr2Image* i, const wchar_t* name, FfxResourceStates state) {
    if (!i->image) {
        FfxResource none;
        memset(&none, 0, sizeof none);
        return none;
    }
    return ffxGetTextureResourceVK(&f->context, i->image, i->view, i->width, i->height, i->format, name, state);
}

int snx_fsr2_dispatch(SnxFsr2* f, const SnxFsr2Dispatch* s) {
    FfxFsr2DispatchDescription d;
    memset(&d, 0, sizeof d);
    d.commandList = ffxGetCommandListVK(s->cb);
    d.color = image(f, &s->color, L"snx_fsr2_color", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.depth = image(f, &s->depth, L"snx_fsr2_depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = image(f, &s->motion, L"snx_fsr2_motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.exposure = image(f, &s->exposure, L"snx_fsr2_exposure", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.reactive = image(f, &s->reactive, L"snx_fsr2_reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.output = image(f, &s->output, L"snx_fsr2_output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = s->jitter[0];
    d.jitterOffset.y = s->jitter[1];
    /* FSR's scale brings a vector to render pixels too (it divides by the render size itself) */
    d.motionVectorScale.x = s->motion_scale[0];
    d.motionVectorScale.y = s->motion_scale[1];
    d.renderSize.width = s->render[0];
    d.renderSize.height = s->render[1];
    d.enableSharpening = s->sharpness > 0;
    d.sharpness = s->sharpness > 1 ? 1 : s->sharpness;
    d.frameTimeDelta = s->frame_ms > 0 ? s->frame_ms : 16.7f;
    d.preExposure = s->pre_exposure > 0 ? s->pre_exposure : 1;
    d.reset = s->reset != 0;
    d.cameraNear = s->near_plane;
    d.cameraFar = s->far_plane;
    d.cameraFovAngleVertical = s->fov_y;
    d.viewSpaceToMetersFactor = 1;
    return (g_snx_fsr2_error = (int)ffxFsr2ContextDispatch(&f->context, &d)) == FFX_OK;
}
