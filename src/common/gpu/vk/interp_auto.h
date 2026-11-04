/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Frame interpolation's Auto mode (the frontends' presenters): whether the game's frames leave room for the blend and
   the frame in one refresh period, and steady enough that the blend does not come and go. The rule is the one
   snx_fx_interp_auto states (the display refreshes at least 1.9 times the game), taken over the game's fastest 1 % of
   frames so two presents always fit, and its 1 % low must stay near its median. A second of fit turns it on, two of
   failure off, so a game at the edge does not switch every frame. */
#ifndef SNX_COMMON_GPU_VK_INTERP_AUTO_H
#define SNX_COMMON_GPU_VK_INTERP_AUTO_H
#include <stdint.h>

#define SNX_INTERP_AUTO_FRAMES 240 /* the frames the 1 % figures are taken over: about four seconds at 60 a second */

typedef struct {
    uint64_t at[SNX_INTERP_AUTO_FRAMES]; /* the frames' times, a ring that next writes on */
    unsigned next, count;
    uint64_t last_ns;    /* the frame before, for the interval */
    uint64_t since_ns;   /* when the figures were last taken */
    uint64_t pending_ns; /* when the condition opposite to on started, 0 while it holds */
    int on;
} SnxInterpAuto;

void snx_interp_auto_init(SnxInterpAuto* auto_);
/* A game frame at now_ns: 1 to present a blend before it. display_hz is the display's refresh, 0 when it is not
   known (then never on) */
int snx_interp_auto_frame(SnxInterpAuto* auto_, uint64_t now_ns, int display_hz);
#endif
