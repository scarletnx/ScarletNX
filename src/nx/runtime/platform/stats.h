/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Stats (settings.txt's stats): the game's frames a second and its last frame's time, the processor's share the
   process takes beside the GPU's busy share and time a frame, the memory it takes of the machine's, the video memory it
   uses of the driver's budget, the quickest and slowest of its last STATS_FRAMES frames' times, and under them a graph
   of those times with a guide at 60 and 30 FPS's, as wide as the lines and right under them, in stats_corner's corner
   over the game; in showcase's own corner
   beside showcase's box, toward the picture's middle. Under the graph, while they apply: RenderDoc's lines, that a
   capture can be taken (the capture key, hotkey_capture) and the captures written with the last one's file, and the
   debugger tracing the game. On every system: a value it does not tell shows as ? */
#ifndef SNX_STATS_H
#define SNX_STATS_H
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/ini.h"

#define STATS_FRAMES 120   /* the frames the graph shows */
#define STATS_COLS   36    /* the box's width, in letters: a line's icon and space take three */
#define STATS_LINES  5     /* its lines of text, each after its icon, the last the graph's title with its times */
#define STATS_GPU_AT 14    /* the letter the GPU's icon starts at, beside the processor's line */
#define STATS_GRAPH_W (STATS_COLS - 2) /* the graph's width in letters: the lines', from the icons to their ends */
#define STATS_TOP_MS 50.0f /* the graph's top; a longer frame reaches it */
/* the graph's two guides, a frame of a 60 FPS game and of a 30 FPS one; a frame past the second draws in the failure
   colour */
#define STATS_60_MS  (1000.0f / 60)
#define STATS_30_MS  (1000.0f / 30)
#define STATS_NOTES  3    /* the lines under the graph at most: RenderDoc's two and the debugger's */
#define STATS_PATH   1024 /* a capture's path, with its NUL */

/* what the renderer reads of the GPU at a frame while stats is on: the time its graphics queue was busy, summed from
   any start (busy_ns), where it can time that queue (timed); the device-local heap's use by the process and the
   driver's budget for it, in bytes, 0 where the driver does not tell them */
typedef struct {
    u64 busy_ns;
    int timed;
    u64 vram, vram_budget;
} SnxStatsGpu;
/* what the measured lines say. cpu: the processor's share of the machine in percent, -1 unknown; memory in bytes, 0
   unknown; memory_total the machine's physical memory, 0 unknown. gpu: the graphics queue's busy share of the time in percent, -1 unknown; gpu_ns its busy time a frame.
   vram and vram_budget as SnxStatsGpu's */
typedef struct {
    int cpu;
    u64 memory, memory_total;
    int gpu;
    u64 gpu_ns;
    u64 vram, vram_budget;
} SnxStatsMeasure;

/* what RenderDoc's lines say (gpu/vk/renderdoc.c, as it loads and after each capture): ready, a capture can be taken
   (RenderDoc is in the process, and the capture key asks for one); taken, the captures written so far; failed, the last
   one asked for wasn't written; path, the last one written's file, "" before one or where RenderDoc didn't say it */
typedef struct {
    int ready;
    unsigned taken;
    int failed;
    char path[STATS_PATH];
} SnxStatsCapture;

/* stats_corner's: tl, tr, bl or br; another value keeps the corner it had, said in the log */
void stats_corner_set(const IniOpt* o, const char* v);
/* a frame of the game presented at now_ns, on the renderer's thread, with what the renderer read of the GPU then:
   kept while stats is on, and once a second the processor, the memory and the GPU measured with it, the GPU's busy
   share and time a frame over that second, and the debugger tracing the game, from the first frame on. Stats off
   forgets them, so it starts again from the next. It takes ui_lock */
void stats_frame(u64 now_ns, const SnxStatsGpu* gpu);
/* what the measured lines say; stats_frame sets it, the tests too */
void stats_measured(const SnxStatsMeasure* m);
/* RenderDoc's lines from now on, kept while stats is off. Any thread; it takes ui_lock */
void stats_capture(const SnxStatsCapture* c);
/* the debugger's line: the debugger tracing the game by name (? where the system doesn't name it), "" for none.
   stats_frame sets it at each measure from snx_debugger, the tests too; stats off forgets it. It takes ui_lock */
void stats_debugger(const char* name);
/* the box's letters' scale, times the 8x16 font, beside the menu's (overlay_place's) menu_scale: a step smaller, 1 at
   least, so the numbers over the game hide less of it than the menu that stops it */
int stats_scale(int menu_scale);
/* the box over a picture whose safe area is safe (x, y, w, h), in stats_scale's letters for the menu's menu_scale, at
   now_ns: nothing while stats is off. It is laid out only when its place, its scale or the lines under the graph
   change: a frame writes the letters of the values that changed into fixed fields of cells (ui_cells), and the bars'
   heights, the same quads each frame. Under ui_lock */
void stats_draw(SnxUiList* l, const int safe[4], int menu_scale, u64 now_ns);
/* the box's place for that in letters of scale (stats_scale's): x, y, w, h into box. STATS_COLS letters wide, or
   wider for a line under the graph that needs it, up to the safe area's width */
void stats_box(const int safe[4], int scale, float box[4]);
/* the box as stats_draw last laid it out (x, y, w, h into box), from its first cell that draws: stats_box's less
   its first column, a margin. 1, or 0 while stats is off or was never drawn. For what else shows over the game and
   keeps clear of it or lines up with it (overlay.c's notification). Under ui_lock */
int stats_box_drawn(float box[4]);
#endif
