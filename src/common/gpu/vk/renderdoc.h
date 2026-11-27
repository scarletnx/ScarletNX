/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc's captures of a console's frames (renderdoc.c): its library loaded by renderdoc= before the Vulkan instance
   exists, or in the process already when RenderDoc's program started the game; the capture key or renderdoc_frames
   then takes a frame, from one present to the next */
#ifndef SNX_VK_RENDERDOC_H
#define SNX_VK_RENDERDOC_H
#include "common/gpu/vk/vk_common.h"
#include "common/cpu/aarch64/cpu.h" /* u64 */

/* RenderDoc's library, by the name its own program puts in a game it starts: the one load looks for and opens */
#if defined(_WIN32)
#define SNX_VK_RENDERDOC_LIBRARY "renderdoc.dll"
#elif defined(__ANDROID__)
#define SNX_VK_RENDERDOC_LIBRARY "libVkLayer_GLES_RenderDoc.so"
#else
#define SNX_VK_RENDERDOC_LIBRARY "librenderdoc.so"
#endif
/* what the runtime gives it, each NULL for none: a line over the game; a line of its log, a failure with warn; its
   run's clock in ms, which renderdoc_frames' times count on; and the frame's commands submitted before a capture ends */
typedef struct {
    void (*notice)(const char* text);
    void (*say)(int warn, const char* line);
    u64 (*clock_ms)(void);
    void (*sync)(void);
} SnxRenderDocHost;
/* before the Vulkan instance is made (its capture layer is an implicit one): RenderDoc's API found, its keys and
   overlay off, renderdoc_frames read, said in the log; the stats' box told a capture can be taken. host is kept */
void snx_renderdoc_load(const SnxRenderDocHost* host);
/* with ask 0 whether a capture can be taken (RenderDoc is in the process); with 1 the frame after the next present
   captured, said in the log and over the game. Any thread */
int snx_renderdoc_capture(int ask);
/* at the present that ends frame `present` of the window's instance, after its last submit and before the swapchain
   present: the capture of this frame ends here, and the next frame's starts */
void snx_renderdoc_present(VkInstance instance, u64 present);
/* snx_renderdoc_load for a console's window: its notice, its log and its clock (common/utilities/vpad_sdl.h's) */
void snx_renderdoc_load_window(void);
#endif
