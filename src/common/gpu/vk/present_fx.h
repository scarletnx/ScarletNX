/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presentation effects every Vulkan frontend shares: the spatial upscaler of the shown frame, anti aliasing of
   the upscaled frame, and the HDR composition and encode of a frame the display shows in HDR. The Switch frontend's
   presenter is the reference, and its FSR 1, NIS, SGSR, FXAA and SMAA shaders are built here for every frontend. A
   frontend hands in its device and the frame slot it is recording, and gets an image to blit or encode. Nothing here
   reads guest memory: the source is a Vulkan image view, and every pass is on the GPU.

   HDR follows the Switch frontend's choice (src/nx/gpu/vk/hdr.c): the shown crop goes into linear light in nits and
   BT.2020's primaries, only the top 20 % of an SDR frame's code range is lifted, the paper white is BT.2408's 203
   nits, and the colours' ratios never move. A frame that already holds HDR data is passed through and tone mapped
   down where the display has no HDR. */
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
/* hdr */
enum { SNX_FX_HDR_OFF, SNX_FX_HDR_AUTO, SNX_FX_HDR_ON, SNX_FX_HDR_COUNT };
/* frame interpolation: off, Auto where the display refreshes at least twice the game's rate, or a blend of the
   previous and current frame. Auto and Blend are the temporal blend until a motion compensated pass lands. */
enum { SNX_FX_INTERP_OFF, SNX_FX_INTERP_AUTO, SNX_FX_INTERP_BLEND, SNX_FX_INTERP_COUNT };
enum { SNX_FX_CS_AUTO, SNX_FX_CS_P3, SNX_FX_CS_SCRGB, SNX_FX_CS_HDR10, SNX_FX_CS_COUNT };
enum { SNX_FX_SRC_AUTO, SNX_FX_SRC_SDR, SNX_FX_SRC_LINEAR, SNX_FX_SRC_PQ, SNX_FX_SRC_COUNT };
/* what the window's surface took: none, HDR10 (PQ), scRGB or Display P3's wide gamut */
enum { SNX_FX_DISPLAY_NONE, SNX_FX_DISPLAY_PQ, SNX_FX_DISPLAY_SCRGB, SNX_FX_DISPLAY_P3 };

/* the .ini's names, and the menu's labels, for the settings every frontend shares */
int snx_fx_upscale_parse(const char* value);
const char* snx_fx_upscale_key(int filter);
const char* snx_fx_upscale_label(int filter);
int snx_fx_aa_parse(const char* value);
const char* snx_fx_aa_key(int aa);
const char* snx_fx_aa_label(int aa);
int snx_fx_hdr_parse(const char* value);
const char* snx_fx_hdr_key(int mode);
const char* snx_fx_hdr_label(int mode);
int snx_fx_colorspace_parse(const char* value);
const char* snx_fx_colorspace_key(int cs);
const char* snx_fx_colorspace_label(int cs);
int snx_fx_source_parse(const char* value);
const char* snx_fx_source_key(int source);
const char* snx_fx_source_label(int source);
int snx_fx_interp_parse(const char* value);
const char* snx_fx_interp_key(int mode);
const char* snx_fx_interp_label(int mode);
/* Auto's answer: the display refreshes at least 1.9 times the game's rate, the point two presented frames per game
   frame fit without the second waiting for a refresh. A display and a game at the same rate is Off, so the game keeps
   its own pace. */
int snx_fx_interp_auto(int display_hz, int game_hz);

/* what a frontend's settings say, read once at start (the same settings the Switch frontend uses) */
typedef struct {
    int upscaler;
    int sharpness;
    int aa;
    int hdr;
    int colorspace;
    int source;
    int paper_nits;
    int peak_nits;
    int expand;
    int test;
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
/* what the window's surface took, and the display's headroom over its SDR white in percent (100 = none) */
void snx_fx_set_display(SnxFx* fx, int kind, int headroom_pct);
/* the live cycles of the hotkeys: the upscaler (FSR 1, Bilinear, NIS, SGSR, None), the anti aliasing (Auto, FXAA,
   SMAA, SSAA 2x, SSAA 4x, Off), and HDR Off and On. The changed value is returned */
int snx_fx_upscale_cycle(SnxFx* fx);
int snx_fx_aa_cycle(SnxFx* fx);
int snx_fx_hdr_toggle(SnxFx* fx);
int snx_fx_hdr_mode(const SnxFx* fx);
int snx_fx_upscaler(const SnxFx* fx);
int snx_fx_aa(const SnxFx* fx);

/* the display kind the surface offers for colorspace (Auto takes P3, scRGB, then PQ), and the format and color space
   to make the swapchain with. SNX_FX_DISPLAY_NONE with *out left alone when it offers none. */
int snx_fx_probe_display(VkPhysicalDevice phys, VkSurfaceKHR surface, int colorspace, VkFormat* out_format,
                         VkColorSpaceKHR* out_space);
/* the kind the display has now: what the surface offered, or none where HDR is off */
int snx_fx_display_kind(const SnxFx* fx);
/* whether HDR runs: On, or Auto and the display takes HDR with headroom over its white */
int snx_fx_hdr_active(const SnxFx* fx);
/* whether a frame of this format already holds HDR data that must not be lifted (a float target is linear light) */
int snx_fx_frame_hdr(VkFormat fmt);
/* the source a frame of this format holds, with cfg->source's override */
int snx_fx_source_for(const SnxFx* fx, VkFormat fmt);
/* whether the frame needs the HDR passes: HDR on, or a frame that already holds HDR data */
int snx_fx_frame_needs(const SnxFx* fx, VkFormat fmt);

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

/* the composition's format (RGBA16F, linear light in nits) */
VkFormat snx_fx_compose_format(void);
/* The shown crop of src into an RGBA16F composition of dst_w by dst_h, on black, in the rectangle (dx, dy) of dw by
   dh, in linear light in nits. The source is in SHADER_READ_ONLY_OPTIMAL; the composition comes back in
   COLOR_ATTACHMENT_OPTIMAL. 0 when it could not run. */
int snx_fx_hdr_compose(SnxFx* fx, VkCommandBuffer cb, VkImageView src, VkFormat src_fmt, uint32_t img_w, uint32_t img_h,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t transform, uint32_t dst_w,
                       uint32_t dst_h, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh, SnxFxImage* out);
/* the composition into dst as enc says: the SDR range, PQ, scRGB or Display P3. dst comes back in
   COLOR_ATTACHMENT_OPTIMAL. */
enum { SNX_FX_ENC_SDR, SNX_FX_ENC_PQ, SNX_FX_ENC_SCRGB, SNX_FX_ENC_P3 };
int snx_fx_hdr_encode(SnxFx* fx, VkCommandBuffer cb, const SnxFxImage* composed, VkImage dst, VkImageView dst_view,
                      VkFormat dst_fmt, uint32_t w, uint32_t h, int enc);
/* the six-patch HDR test band at the bottom of composed */
int snx_fx_hdr_test(SnxFx* fx, VkCommandBuffer cb, const SnxFxImage* composed);
/* the encode kind the display's image takes */
int snx_fx_encode_kind(const SnxFx* fx);

#endif
