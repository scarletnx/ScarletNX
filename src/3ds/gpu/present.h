/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PRESENT_H
#define SNX_CTR_PRESENT_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>
/* The one window's arrangements (ctr_screen_layout), numbered as Azahar's LayoutOption (src/common/settings.h at
   11eceda5): the top screen above the bottom one, one screen alone, the large screen with the other at its right, a
   quarter of that screen's own size, or side by side. ctr_screen_swap trades the screens' places, and picks the
   bottom screen in the one-screen layout. Ctrl+F3 and Ctrl+Shift+F3 change the layout and the swap while the game
   runs */
typedef enum {
    SNX_CTR_LAYOUT_STACKED, /* LayoutOption::Default */
    SNX_CTR_LAYOUT_SINGLE,  /* LayoutOption::SingleScreen */
    SNX_CTR_LAYOUT_LARGE,   /* LayoutOption::LargeScreen */
    SNX_CTR_LAYOUT_SIDE,    /* LayoutOption::SideScreen */
    SNX_CTR_LAYOUT_COUNT
} SnxCtrLayout;
/* The two screens at the console's sizes, in pixels: a frame's size over its render scale */
typedef struct {
    uint32_t top_width, top_height, bottom_width, bottom_height;
} SnxCtrScreens;
/* A screen's rectangle in the window's pixels: its top left corner and its size, both zero for a hidden screen */
typedef struct {
    int32_t x, y, width, height;
} SnxCtrRect;
/* The two screens' rectangles as the layout puts them */
typedef struct {
    SnxCtrRect top, bottom;
} SnxCtrRects;
/* The one window's rectangles for a layout, into rects, at the screens' console sizes and the window's; false for a
   zero window size. A screen of size 0 has no frame yet: it takes the console's size in the layout and gets no
   rectangle, both zero, so the other screen keeps the place both leave it */
bool snx_ctr_layout_rects(SnxCtrLayout layout, bool swap, uint32_t window_width, uint32_t window_height,
                          SnxCtrScreens screens, SnxCtrRects* rects);
/* The shown framebuffers, on the GPU (present.c). init brings up the 3DS's Vulkan device on memory (vk/vk.h) with a
   window when it can make one; without a Vulkan device that can, it fails and nothing is shown. The 3DS display's
   Separate screens setting gives each screen its own window. frame_dir receives bounded PPM dumps (the 3DS framebuffer
   is 240x400/320 portrait, shown rotated 90 degrees clockwise, like the console's landscape screens). */
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir);
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
/* A line for the run's summary on the standard output: each screen's frames decoded, and what the GPU's work took of
   the RAM (vk_render.h's snx_ctr_vk_traffic). Nothing before init */
void snx_ctr_present_report(void);
/* Pumps SDL's events: true once the player closed the window, or SIGINT or SIGTERM came. Esc's first press, the
   upscaler's and the anti-aliasing's shortcuts (the Switch's hotkey_upscaler and hotkey_anti_aliasing), Ctrl+F3's
   screen layout step and Ctrl+Shift+F3's swap are said in a line over the game */
bool snx_ctr_present_closed(void);
/* that line while it shows, "" when none does */
const char* snx_ctr_present_notice(void);
/* the presenter and the device */
void snx_ctr_present_free(void);
#endif
