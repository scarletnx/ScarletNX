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
#include <stddef.h>
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
enum {
    SNX_FX_AA_OFF,
    SNX_FX_AA_AUTO,
    SNX_FX_AA_FXAA,
    SNX_FX_AA_SMAA,
    SNX_FX_AA_SSAA2,
    SNX_FX_AA_SSAA4,
    SNX_FX_AA_COUNT
};

/* the settings' words (upscaler, anti_aliasing; settings.txt's) and the menus' labels: a word case aside, -1 for one
   that is none of them; a value out of the enum has key and label NULL */
int snx_fx_upscale_parse(const char* value);
const char* snx_fx_upscale_key(int filter);
const char* snx_fx_upscale_label(int filter);
int snx_fx_aa_parse(const char* value);
const char* snx_fx_aa_key(int aa);
const char* snx_fx_aa_label(int aa);
/* the shortcuts' step: the next value in the settings' order, the last one's the first */
int snx_fx_upscale_next(int filter);
int snx_fx_aa_next(int aa);

/* what a frontend's settings say, read once at start (the Switch's settings, which every frontend takes) */
typedef struct {
    int upscaler;
    int sharpness; /* upscaler_sharpness: 0 the softest, 100 the sharpest */
    int aa;
} SnxFxConfig;
/* the settings' values into cfg: a word that is none of its setting's is said into said (n bytes, "" when every one
   is known), with the value that runs for it, Off. How many were not known */
int snx_fx_settings(SnxFxConfig* cfg, const char* upscaler, int sharpness, const char* aa, char* said, size_t n);

typedef struct SnxFx SnxFx;

/* an image the module owns, valid until its next render in that slot or until destroy */
typedef struct {
    VkImage image;
    VkImageView view;
    uint32_t width, height;
} SnxFxImage;

/* The device the effects run on, with slots frame slots (snx_fx_render's), each with images of its own. cache may be
   VK_NULL_HANDLE. log is called with a short message; may be NULL. The passes bind pipelines and sets of their own:
   a frontend that keeps the state it bound sets it again after them. */
SnxFx* snx_fx_create(VkPhysicalDevice phys, VkDevice dev, VkPipelineCache cache, uint32_t slots,
                     void (*log)(const char*));
void snx_fx_destroy(SnxFx* fx);
/* the upscalers' sharpness, upscaler_sharpness's 0 (the softest) to 100 (the sharpest), clamped */
void snx_fx_set_sharpness(SnxFx* fx, int sharpness);
/* the device's shaderFloat16, for FSR 1's fp16 passes where it runs */
void snx_fx_set_fp16(SnxFx* fx, int on);
/* whether the frames snx_fx_render is given hold linear light (read through an sRGB view): FXAA then judges an edge
   by the luma the display shows. 0, values as encoded, until set */
void snx_fx_set_light(SnxFx* fx, int linear);

/* Auto anti aliasing: 1 where the shown rectangle is at least 1.25 times the rendered frame in each direction, so
   the upscale makes its edges visible, and where it is not smaller (a downscale already smooths). */
int snx_fx_aa_auto(uint32_t render_w, uint32_t render_h, uint32_t shown_w, uint32_t shown_h);

/* The shown crop of src (img_w by img_h) through the filter and the anti aliasing asked, into an image of out_w by
   out_h, or of the crop's own size where the upscaler is Off and FXAA or SMAA run, which the caller scales as it
   scales the frame: 1 with *out set (its size in it), 0 when neither runs (the caller keeps its own blit, a smooth one
   for any filter but Off).
   What runs: an upscaler where the crop grows, the bilinear copy where it does not; NIS where it grows twice at most,
   else FSR 1; Auto as snx_fx_aa_auto says; SSAA where the device holds the image 2 or 4 times as large, else the
   step under it. The log says what runs and why it is not what was asked, once each time that changes. slot is the
   frame slot cb records (modulo slots), whose last frame the GPU is done with: its images are this frame's. src is in
   SHADER_READ_ONLY_OPTIMAL. transform is vi's flip: bit 0 mirrors x, bit 1 mirrors y. The output is left in
   TRANSFER_SRC_OPTIMAL. */
int snx_fx_render(SnxFx* fx, VkCommandBuffer cb, uint32_t slot, VkImageView src, uint32_t img_w, uint32_t img_h,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t transform, uint32_t out_w, uint32_t out_h,
                  int filter, int aa, SnxFxImage* out);

#endif
