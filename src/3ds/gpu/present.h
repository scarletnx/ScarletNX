/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PRESENT_H
#define SNX_CTR_PRESENT_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>
struct SDL_Window; /* SDL's window, as SDL_video.h names it */
/* The window's arrangements (ctr_screen_layout), numbered as Azahar's LayoutOption (src/common/settings.h at
   11eceda5): the top screen above the bottom one, one screen alone, the large screen with the other at its own size
   over 2.25 (Azahar's Android default), centred vertically at the large one's right, side by side, or separate windows
   (each screen in a window of its own, the one window stacked where the host cannot make the second). ctr_screen_swap
   trades the screens' places, and picks the bottom screen in the one-screen layout */
typedef enum {
    SNX_CTR_LAYOUT_STACKED,  /* LayoutOption::Default */
    SNX_CTR_LAYOUT_SINGLE,   /* LayoutOption::SingleScreen */
    SNX_CTR_LAYOUT_LARGE,    /* LayoutOption::LargeScreen */
    SNX_CTR_LAYOUT_SIDE,     /* LayoutOption::SideScreen */
    SNX_CTR_LAYOUT_SEPARATE, /* LayoutOption::SeparateWindows */
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
   zero window size. A screen of size 0 has no frame yet and gets no rectangle, both zero. The stacked layout gives
   the other screen the whole box. In the one-screen layout the missing screen gets no rectangle, so that window shows
   black until it has a frame; the console's size only keeps the arithmetic defined there. In the large and side
   layouts the other screen keeps its place, the console's size standing in for the missing one in the box */
bool snx_ctr_layout_rects(SnxCtrLayout layout, bool swap, uint32_t window_width, uint32_t window_height,
                          SnxCtrScreens screens, SnxCtrRects* rects);
/* The shown framebuffers, on the GPU (present.c). init brings up the 3DS's Vulkan device on memory (vk/vk.h) with a
   window when it can make one; headless starts no SDL video and makes no window or surface, whatever display the host
   has. Without a Vulkan device that can, it fails and nothing is shown. Screen layout 4 gives each screen its own
   window. frame_dir receives bounded PPM dumps (the 3DS framebuffer is 240x400/320 portrait,
   shown rotated 90 degrees clockwise, like the console's landscape screens). */
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir, bool headless);
/* The window's title, as a Switch game's: the game's name, what it is played with in parentheses when anything (an
   update, DLC), then the build ("ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)"); and the program's icon */
void snx_ctr_present_title(const char* name, const char* addons);
bool snx_ctr_present_ready(void);
/* The screens' window (the top screen's, with screen layout 4) fullscreen, as the Switch's fullscreen=1 starts one.
   F11 or Alt+Enter switches the window it is pressed in, while the game runs */
void snx_ctr_present_fullscreen(void);
/* Live window output is active (SDL3 Vulkan window + swapchain). When false, frames still render but only the dumps
   record them. */
bool snx_ctr_present_windowed(void);
/* Screen layout 4 is on and both windows present: each screen has its own window and swapchain */
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
/* The last frame of both screens into exactly path as one PPM, always the top above the bottom whatever the window's
   layout or swap, on black. A screen that never decoded is absent. The GPU is waited for; false when the file cannot be
   written or no frame was decoded */
bool snx_ctr_present_shot(const char* path);
/* the game's folder (its captures/ for F3) and its .ini (the on-screen controls' and the runtime keys' settings): the
   window then takes the runtime's keys, as every console's (common/utilities/vpad_sdl.h). A windowless run keeps none */
void snx_ctr_present_game(const char* folder, const char* ini);
/* a save state's picture (common/utilities/state_header.h's SNX_STATE_THUMB_W x _H, RGBA) of both screens as --shot
   lays them out, into thumb: false, thumb as it was, where no frame was decoded */
bool snx_ctr_present_thumb(uint8_t* thumb);
/* the runtime's keys taken in window (the game's, or a test's), ini as above: what snx_ctr_present_game opens */
void snx_ctr_present_keys(struct SDL_Window* window, const char* ini);
/* a save states' key pressed since the last call (HOTKEY_SAVE_STATE, _LOAD_STATE, _NEXT_SLOT), -1 for none */
int snx_ctr_present_state_ask(void);
/* whether the thread dump's key (F5) was pressed since the last call: the run lists its threads between dispatches */
bool snx_ctr_present_dump_asked(void);
/* A line for the run's summary on the standard output: each screen's frames decoded, and what the GPU's work took of
   the RAM (vk_render.h's snx_ctr_vk_traffic). Nothing before init */
void snx_ctr_present_report(void);
/* Pumps SDL's events: true once the player closed the window, or SIGINT or SIGTERM came. Esc's first press is said in a
   line over the game */
bool snx_ctr_present_closed(void);
/* that line while it shows, "" when none does */
const char* snx_ctr_present_notice(void);
/* the presenter and the device */
void snx_ctr_present_free(void);
#endif
