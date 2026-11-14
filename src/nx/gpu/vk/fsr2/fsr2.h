/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AMD FidelityFX Super Resolution 2.2.1 (the temporal upscaler, MIT, Copyright (c) 2022-2023 Advanced Micro Devices,
   Inc., https://github.com/GPUOpen-Effects/FidelityFX-FSR2) on any Vulkan device, for the DLSS evaluations of the
   Ounce's games (gpu/nvn/nvn_dlss.c). AMD's library and its Vulkan backend are built as they are; this is the C
   interface the runtime and tests/vk_fsr2.c call. Nothing here knows the renderer: the caller gives the images, in the
   layouts said, and the command buffer the passes are recorded into with plain vkCmd* calls */
#ifndef SNX_NX_GPU_VK_FSR2_H
#define SNX_NX_GPU_VK_FSR2_H
#include <stdint.h>
#include "volk.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnxFsr2 SnxFsr2;

/* what a context is made for; another value of any of them is another context */
enum {
    SNX_FSR2_HDR = 1,            /* the color is linear and unbounded (a float format), not display-referred */
    SNX_FSR2_DISPLAY_MOTION = 2, /* the motion vectors are at the output's size, not the render's */
    SNX_FSR2_JITTERED_MOTION = 4, /* the motion vectors include the jitter */
    SNX_FSR2_INVERTED_DEPTH = 8, /* depth 1 is the near plane */
    SNX_FSR2_AUTO_EXPOSURE = 32  /* FSR computes the exposure: no exposure image is given */
};
typedef struct {
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t render_max[2]; /* the largest render size a dispatch gives */
    uint32_t output[2];
    uint32_t flags;
} SnxFsr2Info;

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
} SnxFsr2Image;
/* A dispatch. color, depth and motion are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and stay so (so are exposure and
   reactive, which may be absent: image VK_NULL_HANDLE); output is in VK_IMAGE_LAYOUT_GENERAL and stays so. Its view
   has the storage usage. render: the size of the picture in color, depth and motion, from their corner.
   jitter: the offset the camera was moved by, in render pixels (the picture moved by +jitter). motion_scale: what
   brings a motion vector's value to pixels of the motion vectors' size, the render size or with
   SNX_FSR2_DISPLAY_MOTION the output's, as FSR's own scale does; a vector points from a pixel to where it was the
   frame before */
typedef struct {
    VkCommandBuffer cb;
    SnxFsr2Image color, depth, motion, exposure, reactive, output;
    uint32_t render[2];
    float jitter[2], motion_scale[2];
    float pre_exposure;
    float frame_ms;
    float near_plane, far_plane, fov_y; /* fov_y in radians */
    int reset;
    float sharpness; /* 0 none (no sharpening pass), up to 1 */
} SnxFsr2Dispatch;

SnxFsr2* snx_fsr2_create(const SnxFsr2Info* info); /* NULL when the device can't run it */
void snx_fsr2_destroy(SnxFsr2* f);                 /* the device idle of f's commands */
int snx_fsr2_dispatch(SnxFsr2* f, const SnxFsr2Dispatch* d); /* 1 when recorded */
/* FSR's own error code (FfxErrorCode) of the last call that failed */
int snx_fsr2_error(void);
#ifdef __cplusplus
}
#endif
#endif
