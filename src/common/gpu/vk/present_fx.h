/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presentation effects every Vulkan frontend shares: the spatial upscaler of the shown frame and anti aliasing of
   the upscaled frame. The Switch frontend's presenter is the reference, and its FSR 1, NIS, SGSR, FXAA and SMAA
   shaders are built here for every frontend. A frontend hands in its device and the frame slot it is recording, and
   gets an image to blit. Nothing here reads guest memory: the source is a Vulkan image view, and every pass is on the
   GPU. */
#ifndef SNX_COMMON_GPU_VK_PRESENT_FX_H
#define SNX_COMMON_GPU_VK_PRESENT_FX_H
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

/* the spatial filter of the shown frame */
enum {
    SNX_FX_UPSCALE_OFF,
    SNX_FX_UPSCALE_BILINEAR,
    SNX_FX_UPSCALE_FSR1,
    SNX_FX_UPSCALE_NIS,
    SNX_FX_UPSCALE_SGSR,
    SNX_FX_UPSCALE_COUNT
};
/* the anti aliasing of the upscaled frame. Auto runs FXAA only where the display is at least 1.25 times the rendered
   frame in each direction, the point where an upscale starts to make an edge visible (the ratio dynamic resolution
   scalers use to decide that a lower render size needs help). Supersampling renders the pass at 2x or 4x and
   downsamples, which also removes the edges a post process cannot see. */
enum { SNX_FX_AA_OFF, SNX_FX_AA_AUTO, SNX_FX_AA_FXAA, SNX_FX_AA_SMAA, SNX_FX_AA_SSAA2, SNX_FX_AA_SSAA4, SNX_FX_AA_COUNT };

/* the .ini's names, and the menu's labels, for the settings every frontend shares */
int snx_fx_upscale_parse(const char* value);
const char* snx_fx_upscale_key(int filter);
const char* snx_fx_upscale_label(int filter);
int snx_fx_aa_parse(const char* value);
const char* snx_fx_aa_key(int aa);
const char* snx_fx_aa_label(int aa);

/* what a frontend's settings say, read once at start (the same settings the Switch frontend uses) */
typedef struct {
    int upscaler;
    int sharpness;
    int aa;
} SnxFxConfig;

typedef struct SnxFx SnxFx;

/* an image the module owns, valid until its next render in that slot or until destroy */
typedef struct {
    VkImage image;
    VkImageView view;
    uint32_t width, height;
} SnxFxImage;

/* The device the effects run on, and the frame slot (0 to slots - 1) the caller is recording, which keeps each slot's
   images apart. cache may be VK_NULL_HANDLE. slot is called once for each render. forget, when not NULL, is called
   after a rendering pass, for a frontend whose state cache must not keep the module's pipelines. log is called with
   a short message; may be NULL. */
SnxFx* snx_fx_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, VkPipelineCache cache,
                     uint32_t slots, uint32_t (*slot)(void), void (*forget)(void), void (*log)(const char*));
void snx_fx_destroy(SnxFx* fx);
void snx_fx_configure(SnxFx* fx, const SnxFxConfig* cfg);
/* the device's shaderFloat16, for FSR 1's fp16 passes where it runs */
void snx_fx_set_fp16(SnxFx* fx, int on);
/* the anti aliasing now, for a live key that changes it */
void snx_fx_set_aa(SnxFx* fx, int aa);
/* the live cycles of the hotkeys: the upscaler (FSR 1, Bilinear, NIS, SGSR, None) and the anti aliasing (Auto, FXAA,
   SMAA, SSAA 2x, SSAA 4x, Off). The changed value is returned */
int snx_fx_upscale_cycle(SnxFx* fx);
int snx_fx_aa_cycle(SnxFx* fx);
int snx_fx_upscaler(const SnxFx* fx);
int snx_fx_aa(const SnxFx* fx);

/* the filter the frame runs: an unavailable filter is replaced by FSR 1, then Bilinear; -1 for no source */
int snx_fx_pick_filter(SnxFx* fx, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t transform);
/* whether the filter exists on this device */
int snx_fx_filter_available(SnxFx* fx, int filter);
/* whether the filter uses sharpness */
int snx_fx_filter_uses_sharpness(int filter);
/* the upscaler's target from the screen rectangle and upscaler_mult, at most max_dim each axis */
uint32_t snx_fx_target(uint32_t w, uint32_t h, uint32_t mult, uint32_t max_dim, uint32_t* out_w, uint32_t* out_h);
/* Auto anti aliasing: 1 where the shown rectangle is at least 1.25 times the rendered frame in each direction, so
   the upscale makes its edges visible, and where it is not smaller (a downscale already smooths). */
int snx_fx_aa_auto(uint32_t render_w, uint32_t render_h, uint32_t shown_w, uint32_t shown_h);
/* the anti aliasing a frame runs: Auto resolves to FXAA or Off; SSAA2/4 when the device can hold the larger image */
int snx_fx_pick_aa(SnxFx* fx, int aa, uint32_t render_w, uint32_t render_h, uint32_t shown_w, uint32_t shown_h,
                   uint32_t max_dim);

/* The shown crop of src (img_w by img_h) through the spatial filter and the anti aliasing, into an image of out_w by
   out_h: 1 with *out set, 0 when neither runs (the caller keeps its own blit). src is in
   SHADER_READ_ONLY_OPTIMAL. transform is vi's flip: bit 0 mirrors x, bit 1 mirrors y. The output is left in
   TRANSFER_SRC_OPTIMAL. */
int snx_fx_render(SnxFx* fx, VkCommandBuffer cb, VkImageView src, uint32_t img_w, uint32_t img_h, uint32_t x,
                  uint32_t y, uint32_t w, uint32_t h, uint32_t transform, uint32_t out_w, uint32_t out_h, int filter,
                  int aa, SnxFxImage* out);

#endif
