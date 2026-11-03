/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HDR of the presented frame (hdr_select.h for the setting). The presenter runs the frame's shown crop into a
   composition image of linear light in nits, BT.2020's primaries, which the on-screen controls and the game's menu draw
   over in the same space, and then encodes into the window's image: PQ (HDR10) when the display takes it, else the SDR
   range with a knee. A frame that already holds HDR data (a float target, or the hdr_source setting) is never lifted
   again. Under vk_lock, in the frame being recorded. */
#ifndef SNX_GPU_VK_HDR_H
#define SNX_GPU_VK_HDR_H
#include "nx/gpu/vk/hdr_select.h"
#include "nx/gpu/vk/vk.h"

/* the composition's format (RGBA16F, linear light in nits), which the on-screen controls draw to */
VkFormat vk_hdr_compose_format(void);
/* the settings and the live mode, read once at start: mode is SNX_HDR_OFF or ON, source SNX_HDR_SRC_*, the paper
   white and peak in nits, the expansion 0 to 100 (the share of the display's headroom the top of an SDR frame is
   lifted into: 0 leaves it exactly as SDR), and test (1 draws the six-patch pattern at the bottom, hdr_test) */
void vk_hdr_configure(int mode, int source, int paper_nits, int peak_nits, int expand, int test);
/* the paper white and the peak the settings name, clamped */
int vk_hdr_paper_nits(void);
int vk_hdr_peak_nits(void);
/* whether the device can run the passes (the composition's 16-bit image) */
int vk_hdr_available(void);
/* what the window's surface takes for an HDR image: 0 none, 1 HDR10 (PQ), 2 scRGB, 3 Display P3's wide gamut. What
   the toast says as HDR is turned on */
enum { SNX_HDR_DISPLAY_NONE, SNX_HDR_DISPLAY_PQ, SNX_HDR_DISPLAY_SCRGB, SNX_HDR_DISPLAY_P3 };
int vk_hdr_display_kind(void);
/* the setting's mode, which the hdr_key hotkey moves (atomic: the window thread changes it) */
int vk_hdr_mode(void);
/* whether HDR runs now: On, or Auto and the display takes an HDR image with headroom over its white */
int vk_hdr_active(void);
/* whether the display takes an HDR image with headroom over its white (what Auto tests) */
int vk_hdr_display_hdr(void);
/* Off to On and back; the mode now */
int vk_hdr_toggle(void);
/* what the toast says of the mode, with what the display took: "On (scRGB, paper white 200 nits, peak 1000)",
   "On (HDR10, ...)", or "Off" */
void vk_hdr_describe(int display, char* out, size_t n);
/* the source a frame of this format holds, with hdr_source's override (SNX_HDR_SRC_SDR, LINEAR or PQ) */
int vk_hdr_source_for(VkFormat fmt);
/* whether the frame holds HDR data that must not be lifted, or is presented as HDR */
int vk_hdr_frame_hdr(VkFormat fmt);
/* whether the frame needs the passes: HDR on, or a frame that already holds HDR data (which is then tone mapped) */
int vk_hdr_frame_needs(VkFormat fmt);
/* the frame's shown crop into the composition of dst_w x dst_h, on black, in the rectangle (dx, dy) of dw x dh, its
   own size: the shown crop fills that rectangle and nothing else, so the letterbox is the frame's aspect, not the
   window's. Flipped as transform says (Android's flags). The source is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   the composition comes back in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. 0 when it could not run */
int vk_hdr_compose(VkCommandBuffer cb, VkImageView src, int source, u32 img_w, u32 img_h, u32 x, u32 y, u32 w, u32 h,
                   u32 transform, u32 dst_w, u32 dst_h, i32 dx, i32 dy, u32 dw, u32 dh, VkImage* out,
                   VkImageView* out_view);
/* what vk_hdr_encode writes: the SDR range (the knee), PQ for an HDR10 target, or the extended linear light of an
   scRGB target (sRGB's primaries) or a Display P3 one (P3's), 1.0 at the paper white */
enum { VK_HDR_ENC_SDR, VK_HDR_ENC_PQ, VK_HDR_ENC_SCRGB, VK_HDR_ENC_P3 };
/* the composition into dst, as enc says: dst comes back in COLOR_ATTACHMENT_OPTIMAL. 0 when it could not run */
int vk_hdr_encode(VkCommandBuffer cb, VkImage composed, VkImageView composed_view, u32 w, u32 h, VkImage dst,
                  VkImageView dst_view, VkFormat dst_fmt, int enc);
/* the window's surface went: its images go too. The device must be idle and vk_lock held */
void vk_hdr_release(void);
#endif
