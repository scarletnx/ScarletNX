/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The frame rate overlay of the frontends with no menu of their own (the 3DS, the Vita and the GBA), on F6: the game's
   frames a second, the generated frames beside them when frame interpolation runs, their total, the frame time, and the
   buttons the player holds. The Switch has its own (runtime/platform/stats.c, which also measures the processor). */
#ifndef SNX_COMMON_GPU_VK_STATS_H
#define SNX_COMMON_GPU_VK_STATS_H
#include "common/gpu/vk/text.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t frames;    /* the game's frames presented since the overlay went on */
    uint64_t generated; /* the generated frames presented beside them (frame interpolation's blends) */
    uint64_t since_ns;  /* the half second the rates are counted from */
    uint64_t since, since_gen;
    uint64_t last_ns, prev_ns; /* the two last frames' times, for the frame time */
    int fps10, gen10;          /* tenths of a frame a second over the last half second; -1 before the first */
    int on;                    /* F12's: the rates and the frame time alone */
    int showcase;              /* F6's: the same and the buttons held */
} SnxStats;

/* a button of the frontend's input and the word the overlay gives it */
typedef struct {
    uint32_t bit;
    const char* name;
} SnxStatsButton;

/* the overlay off, its rates unknown */
void snx_stats_init(SnxStats* stats);
/* a game frame shown at now_ns: the rate's half second moves on, the frame time from the one before */
void snx_stats_frame(SnxStats* stats, uint64_t now_ns);
/* a generated frame shown (frame interpolation's blend) */
void snx_stats_generated(SnxStats* stats);
/* on to off, or off to on from nothing: showcase picks F6's overlay (the rates and the held buttons) or F12's (the
   rates alone); the state of that one after */
int snx_stats_toggle(SnxStats* stats, int showcase);
/* either overlay shows */
int snx_stats_visible(const SnxStats* stats);
/* the rate line: "60.0 fps  60.0 gen  120.0 total  16.7 ms", the generated and total rates only with frame
   interpolation, ? for what is not known yet */
void snx_stats_line(SnxStats* stats, uint64_t now_ns, char* out, size_t size);
/* the held buttons' names, "A B L", or "-" for none */
void snx_stats_inputs(char* out, size_t size, uint32_t held, const SnxStatsButton* buttons, unsigned count);
/* Draws the rates, and the inputs line when it is given, at the target's bottom left through text, with its box, while
   either overlay shows. white and slot are snx_text_draw's. 0 when both are off or nothing was drawn. */
int snx_stats_draw(SnxText* text, SnxStats* stats, VkCommandBuffer cb, VkImageView view, VkFormat format,
                   uint32_t width, uint32_t height, float white, uint32_t slot, uint64_t now_ns, const char* inputs);
#endif
