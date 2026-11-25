/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's upscalers behind one interface (docs/layers/gpu/upscalers.md): a caller gives a picture at a render
   size, with whatever else it has (depth, motion vectors, exposure, jitter), and gets it at an output size, from the
   backend the settings prefer among those that can take what it gives. The caller never names a technology: it says
   which kinds it accepts (temporal, spatial, the filtered copy), and with none of those able to run it is told so, and
   runs its own fallback (a DLSS evaluation's front end copy, a hook module's game chain).

   Like fsr2.h, nothing here knows the renderer, the settings or the log: the caller gives the device, the images in
   the layouts said and the command buffer the passes are recorded into with plain vkCmd* calls, and gets a reason in
   words when something can't run. tests/vk_upscaler.c runs it on a device with no renderer */
#ifndef SNX_NX_GPU_VK_UPSCALER_H
#define SNX_NX_GPU_VK_UPSCALER_H
#include <stdint.h>
#include "volk.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The backends. The order of Best (SNX_UPSCALER_BEST) is DLSS, newer FSR, XeSS, FSR 2, spatial, copy: the first that
   can run takes the call */
typedef enum {
    SNX_UPSCALER_FSR2,    /* AMD FidelityFX Super Resolution 2.2.1 (gpu/vk/fsr2), temporal */
    SNX_UPSCALER_SPATIAL, /* AMD FidelityFX Super Resolution 1's EASU, then its RCAS when sharpening, one frame */
    SNX_UPSCALER_COPY,    /* a bilinear filtered copy of the picture */
    SNX_UPSCALER_DLSS,    /* NVIDIA DLSS Super Resolution through Streamline (dlss.h): Windows, NVIDIA's runtime */
    SNX_UPSCALER_FSR_NEW, /* FSR 3.1 or 4 through AMD's FFX API: not built in yet, never available */
    SNX_UPSCALER_XESS,    /* Intel XeSS: not built in yet, never available */
    SNX_UPSCALERS,
    SNX_UPSCALER_BEST = -1 /* no preference: Best's order */
} SnxUpscalerId;

/* kinds, one a backend, and a caller accepts any of them */
enum { SNX_UP_TEMPORAL = 1, SNX_UP_SPATIAL = 2, SNX_UP_COPY = 4 };
/* the inputs besides the color */
enum { SNX_UP_DEPTH = 1, SNX_UP_MOTION = 2, SNX_UP_JITTER = 4, SNX_UP_EXPOSURE = 8, SNX_UP_REACTIVE = 16 };
/* the color's range: display-referred (0 to 1, possibly with a transfer) or linear and unbounded */
enum { SNX_UP_LDR = 1, SNX_UP_HDR = 2 };

typedef struct {
    const char* name;           /* as the log says it: "FSR 2.2.1" */
    uint32_t kind;              /* one of SNX_UP_TEMPORAL, SNX_UP_SPATIAL, SNX_UP_COPY */
    uint32_t needs;             /* the inputs it can't run without */
    uint32_t takes;             /* the inputs it uses when given (needs among them) */
    uint32_t range;             /* the colors it takes, SNX_UP_LDR and/or SNX_UP_HDR */
    float scale_min, scale_max; /* the output's size over the render size, on each axis */
    int sharpens;               /* sharpness does something */
    int filtered;               /* it samples the color with a linear filter: its format must filter so. Every
                                   backend stores its output, whose format the device must store */
} SnxUpscalerCaps;

const SnxUpscalerCaps* snx_upscaler_caps(SnxUpscalerId id);
/* 1 when backend id is built in and the device can run it, else 0 and why */
int snx_upscaler_available(SnxUpscalerId id, VkPhysicalDevice physical, const char** why);

/* What a call has: the inputs given (SNX_UP_DEPTH...), the kinds the caller accepts, the color's range, the sizes, and
   the color's and the output's formats (VK_FORMAT_UNDEFINED: not checked) */
typedef struct {
    uint32_t given, accept, range;
    uint32_t render[2], output[2];
    VkFormat color, output_format;
} SnxUpscaleNeeds;
/* The fallback rule: the preferred backend, then Best's order, the first that the caller accepts, that is available,
   whose needs the call gives, that takes the color's range, whose scale range holds the sizes and whose formats the
   device takes (the output stored, the color filtered where the backend filters it). SNX_UPSCALERS when
   none: the caller's own fallback runs, and why says what the preferred one (or the last tried) lacked */
SnxUpscalerId snx_upscaler_choose(SnxUpscalerId preferred, VkPhysicalDevice physical, const SnxUpscaleNeeds* n,
                                  const char** why);

/* conventions a context is made for (fsr2.h's bits, the same values) */
enum {
    SNX_UP_FLAG_HDR = 1,             /* the color is linear and unbounded, not display-referred */
    SNX_UP_FLAG_DISPLAY_MOTION = 2,  /* the motion vectors are at the output's size, not the render's */
    SNX_UP_FLAG_JITTERED_MOTION = 4, /* the motion vectors include the jitter */
    SNX_UP_FLAG_INVERTED_DEPTH = 8,  /* depth 1 is the near plane */
    SNX_UP_FLAG_AUTO_EXPOSURE = 32   /* the backend computes the exposure: no exposure image is given */
};
typedef struct {
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t render_max[2]; /* the largest render size an evaluation gives */
    uint32_t output[2];
    VkFormat color;         /* the color's, as the first evaluation gives it (the choice checked it) */
    VkFormat output_format; /* the output view's, one the device stores */
    uint32_t flags;
} SnxUpscalerInfo;

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
} SnxUpscaleImage;
/* An evaluation. color, depth, motion, exposure and reactive are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and stay
   so. Those not given have image VK_NULL_HANDLE (a backend that needs one refuses the call). output is in
   VK_IMAGE_LAYOUT_GENERAL with a storage view, and stays so: the backend writes its whole output size. render: the
   picture's size in the inputs, from their corner. jitter: the offset the camera was moved by, in render pixels (the
   picture moved by +jitter). motion_scale: what brings a motion vector to pixels of the motion vectors' size, and a
   vector points from a pixel to where it was the frame before. The camera's planes and field of view (radians) only
   weigh depth comparisons: 0 takes 0.1, 1000 and 1 */
typedef struct {
    VkCommandBuffer cb;
    SnxUpscaleImage color, depth, motion, exposure, reactive, output;
    uint32_t render[2];
    float jitter[2], motion_scale[2];
    float pre_exposure;
    float frame_ms;
    float near_plane, far_plane, fov_y;
    int reset;
    float sharpness; /* 0 none (no sharpening pass), up to 1 */
    /* The copy's and the spatial upscaler's alone (a temporal one resolves edges and jitter itself): smaa runs SMAA on
       the picture at the render size before the upscale, 1 for SMAA 1x, 2 for T2x (1x, then a resolve with the frame
       before, reprojected by the motion vectors: without them it is 1x. Its caller gives the jitter as shift, so both
       frames are taken back by theirs and the resolve is steady), at smaa_quality (0 to 3: SMAA's Low, Medium,
       High, Ultra) and with edges found by smaa_edges (0 luma, 1 color, 2 depth: without a depth image of our render
       size's largest, luma). shift takes the picture back by that many render pixels first (the camera's jitter, for a
       steady picture). 0 for neither */
    int smaa, smaa_quality, smaa_edges;
    float shift[2];
} SnxUpscaleEval;

typedef struct SnxUpscaler SnxUpscaler;
/* a context of backend id for info: NULL and why when it can't be made */
SnxUpscaler* snx_upscaler_create(SnxUpscalerId id, const SnxUpscalerInfo* info, const char** why);
/* the passes of one evaluation into e->cb: 1 when recorded, else 0 and why. A context records into at most
   SNX_UPSCALER_IN_FLIGHT command buffers that haven't finished: its descriptors go round that many */
int snx_upscaler_evaluate(SnxUpscaler* u, const SnxUpscaleEval* e, const char** why);
enum { SNX_UPSCALER_IN_FLIGHT = 4 };
/* the device done with the context's commands, its objects freed */
void snx_upscaler_destroy(SnxUpscaler* u);
SnxUpscalerId snx_upscaler_id(const SnxUpscaler* u);
#ifdef __cplusplus
}
#endif
#endif
