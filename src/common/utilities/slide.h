/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A line too long for its room slides a column every step_ms and back, resting SNX_SLIDE_WAIT steps at each end: the
   launcher's rows (gui_slide) and the runtime's Debug list (hud.c) alike */
#ifndef SNX_COMMON_SLIDE_H
#define SNX_COMMON_SLIDE_H
#include <stdint.h>

#define SNX_SLIDE_WAIT 8 /* steps a sliding line rests at each end */
/* the columns a line over columns too long skips at ms: 0 for none over, or no step */
static inline int snx_slide(int over, uint64_t ms, uint32_t step_ms) {
    uint64_t t;
    if (over <= 0 || !step_ms) return 0;
    t = ms / step_ms % (uint64_t)(2 * (over + SNX_SLIDE_WAIT));
    if (t < SNX_SLIDE_WAIT) return 0;
    if (t < (uint64_t)(SNX_SLIDE_WAIT + over)) return (int)(t - SNX_SLIDE_WAIT);
    if (t < (uint64_t)(2 * SNX_SLIDE_WAIT + over)) return over;
    return over - (int)(t - 2 * SNX_SLIDE_WAIT - over);
}
#endif
