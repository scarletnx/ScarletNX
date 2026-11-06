/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Stats (settings.txt's stats): the game's frames a second and its last frame's time, the processor and the memory
   the process takes, the GPU's busy share and time a frame, and a graph of the times of its last STATS_FRAMES frames,
   in stats_corner's corner over the game; in showcase's own corner beside showcase's box, toward the picture's middle.
   On every system: a value it does not tell shows as ? */
#ifndef SNX_STATS_H
#define SNX_STATS_H
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/ini.h"

#define STATS_FRAMES 120   /* the frames the graph shows */
#define STATS_COLS   24    /* the box's width, in letters */
#define STATS_LINES  3     /* its lines of text, above the graph */
#define STATS_TOP_MS 50.0f /* the graph's top; a longer frame reaches it */
/* the graph's two guides, a frame of a 60 FPS game and of a 30 FPS one; a frame past the second draws in the failure
   colour */
#define STATS_60_MS  (1000.0f / 60)
#define STATS_30_MS  (1000.0f / 30)

/* what the renderer reads of the GPU at a frame while stats is on: the time its graphics queue was busy, summed from
   any start (busy_ns), where it can time that queue (timed) */
typedef struct {
    u64 busy_ns;
    int timed;
} SnxStatsGpu;
/* what the measured lines say. cpu: the processor's share of the machine in percent, -1 unknown; memory in bytes, 0
   unknown. gpu: the graphics queue's busy share of the time in percent, -1 unknown; gpu_ns its busy time a frame */
typedef struct {
    int cpu;
    u64 memory;
    int gpu;
    u64 gpu_ns;
} SnxStatsMeasure;

/* stats_corner's: tl, tr, bl or br; another value keeps the corner it had, said in the log */
void stats_corner_set(const IniOpt* o, const char* v);
/* a frame of the game presented at now_ns, on the renderer's thread, with what the renderer read of the GPU then:
   kept while stats is on, and once a second the processor, the memory and the GPU measured with it, the GPU's busy
   share and time a frame over that second. Stats off forgets them, so it starts again from the next. It takes
   ui_lock */
void stats_frame(u64 now_ns, const SnxStatsGpu* gpu);
/* what the measured lines say; stats_frame sets it, the tests too */
void stats_measured(const SnxStatsMeasure* m);
/* the box over a picture whose safe area is safe (x, y, w, h), in letters of scale times the 8x16 font (the menu's,
   overlay_place), at now_ns: nothing while stats is off. Under ui_lock */
void stats_draw(SnxUiList* l, const int safe[4], int scale, u64 now_ns);
/* the box's place for that: x, y, w, h into box */
void stats_box(const int safe[4], int scale, float box[4]);
#endif
