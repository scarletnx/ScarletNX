/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PRESENT_H
#define SNX_CTR_PRESENT_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>
/* The shown framebuffers, on the GPU (present.c). init brings up the 3DS's Vulkan device on memory (vk/vk.h) with a
   window when it can make one; headless starts no SDL video and makes no window or surface, whatever display the host
   has. Without a Vulkan device that can, it fails and nothing is shown. The 3DS display's Separate screens setting
   gives each screen its own window. frame_dir receives bounded PPM dumps (the 3DS framebuffer is 240x400/320 portrait,
   shown rotated 90 degrees clockwise, like the console's landscape screens). */
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir, bool headless);
/* The window's title, as a Switch game's: the game's name, what it is played with in parentheses when anything (an
   update, DLC), then the build ("ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)"); and the program's icon */
void snx_ctr_present_title(const char* name, const char* addons);
bool snx_ctr_present_ready(void);
/* The screens' window (the top screen's, with Separate screens) fullscreen, as the Switch's fullscreen=1 starts one.
   F11 or Alt+Enter switches the window it is pressed in, while the game runs */
void snx_ctr_present_fullscreen(void);
/* Live window output is active (SDL3 Vulkan window + swapchain). When false, frames still render but only the dumps
   record them. */
bool snx_ctr_present_windowed(void);
/* Separate screens is on and both windows present: each screen has its own window and swapchain */
bool snx_ctr_present_separate(void);
/* The framebuffer at physical address pa, as the guest has it: width pixels a row (240), height rows, pitch bytes a
   row, in GSP's framebuffer formats: 0 = RGBA8 (A,B,G,R bytes, libctru RGBA8_OES), 1 = RGB8 (B,G,R bytes,
   BGR8_OES), 2 = RGB565 (little-endian 16-bit, R in bits 11..15, G 5..10, B 0..4), 3 = RGB5A1, 4 = RGBA4, decoded
   on the GPU, after the GPU's work recorded before it (which may have drawn the frame). hash, when not NULL, gets the
   FNV-1a hash of the decoded RGBA pixels in the framebuffer's order (the tests'): that frame is read back */
bool snx_ctr_present_frame(unsigned screen, uint32_t pa, unsigned width, unsigned height, unsigned format,
                           unsigned pitch, uint64_t* hash);
/* The console's frames decoded so far: each top screen frame, or each bottom screen frame until the first top screen frame */
uint64_t snx_ctr_present_frames(void);
/* The last frame of both screens into exactly path as one PPM, the top above the bottom as the window shows them, on
   black. A screen that never decoded is absent. The GPU is waited for; false when the file cannot be written or no
   frame was decoded */
bool snx_ctr_present_shot(const char* path);
/* A line for the run's summary on the standard output: each screen's frames decoded, and what the GPU's work took of
   the RAM (vk_render.h's snx_ctr_vk_traffic). Nothing before init */
void snx_ctr_present_report(void);
/* Pumps SDL's events: true once the player closed the window, or SIGINT or SIGTERM came */
bool snx_ctr_present_closed(void);
/* the presenter and the device */
void snx_ctr_present_free(void);
#endif
