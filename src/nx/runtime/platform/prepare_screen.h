/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The screen of the shaders prepared before the game starts (main.c's prepare_wait), drawn as the launcher draws its
   loading screen (src/gui/main.c render_steps), in its theme's [run] look, as quads over everything else (ui.h) */
#ifndef SNX_PREPARE_SCREEN_H
#define SNX_PREPARE_SCREEN_H
#include "nx/runtime/platform/ui.h"
#include "common/utilities/run_theme.h"

#define SNX_PREPARE_NOTES     48
#define SNX_PREPARE_NOTE_SIZE 256 /* a line's bytes, the row's width cutting it */
#define SNX_PREPARE_STEPS     4
typedef struct {
    const char* label; /* NULL for none */
    /* a detail's bytes: a count of files and a name (RENDERER_FILE_SIZE) */
#define SNX_PREPARE_DETAIL_SIZE 128
    char detail[SNX_PREPARE_DETAIL_SIZE];
    u32 value, total; /* total 0: no progress to show */
    u64 started_ms;   /* when it began, for the theme's time left */
    int done;
} SnxPrepareStep;
/* The screen as it is now: theme's look (NULL for the YoRHa theme's), the theme's title and the game's name (name),
   a row for each of the n steps, the bar of the first one not done, the latest lines of the work under the steps
   (notes, oldest first: the newest SNX_PREPARE_NOTES are kept), and foot at the bottom. n 0 hides the screen.
   now_ms: the time of the update, a monotonic clock's. Under ui_lock; the strings are copied, NULL ones as "" */
void prepare_screen_set(const SnxRunTheme* theme, const char* name, const SnxPrepareStep* steps, int n,
                        const char (*notes)[SNX_PREPARE_NOTE_SIZE], int nnotes, const char* foot, u64 now_ms);
struct RendererPrepare;
/* the kinds of step prepare_screen_steps makes, by their place in its began */
enum { SNX_PREPARE_FIND, SNX_PREPARE_TRANSLATE, SNX_PREPARE_PIPES, SNX_PREPARE_KINDS };
/* The steps for the renderer's progress p (gpu/renderer.h) into steps, SNX_PREPARE_KINDS at most, each once it has
   work: the files looked in for the game's shaders (how many, and the one looked in now), the shaders translated, and
   the pipelines of earlier runs made. began: when each kind of step was first seen at work, 0 until it is (then set to
   now_ms), for the theme's time left. Their count */
int prepare_screen_steps(const struct RendererPrepare* p, SnxPrepareStep* steps, u64 began[SNX_PREPARE_KINDS],
                         u64 now_ms);
#define PREPARE_SCREEN_MENU_QUADS 1536 /* the quads its scrolling lines leave for the menu: its widest page's */
/* A program a collection's program started (ExecuteProgram; main.c's transition screen): the screen also shows while
   the game has drawn nothing yet, with no step, the bar only while a step shows. program: the one that starts, named
   after the game as "(Program N)" when it is not 0; -1 takes this part away, the screen then showing while it has
   steps. theme, name, foot and now_ms as prepare_screen_set takes them. Under ui_lock */
void prepare_screen_switch(const SnxRunTheme* theme, const char* name, int program, const char* foot, u64 now_ms);
/* the game's cover under the switch's head, from its icon's JPEG (size bytes; NULL for none): drawn SNX_PREPARE_COVER
   texels square into the left of the layer's one picture (ui_image), which it takes while the switch's part is up and
   the menu is not over it. 0, or 1 for a JPEG that does not decode, which shows none. Under ui_lock */
#define SNX_PREPARE_COVER 180 /* ui.h's SNX_UI_IMAGE_H: the picture's height */
int prepare_screen_cover(const void* jpeg, size_t size);
/* the game's frame is shown (the renderer's present, before it makes the frame's layer): a switch's part gives way,
   so the game's first frame is drawn without it. Under ui_lock */
void prepare_screen_game_drew(void);
/* whether it is up (under ui_lock): the on-screen controls aren't drawn under it, nor take fingers */
int prepare_screen_shown(void);
/* it, on a picture sw x sh whose part clear of the system's bars and cutouts is safe (x, y, w, h), its letters the
   launcher's size: the theme's text_size, or with none twice display_percent (the display's scale, overlay.h's), as
   many rows as the launcher keeps fitting the height; the menu goes over it. Under ui_lock. Its quads come
   first and its scrolling lines last, which take only what the list has left but room for the menu */
void prepare_screen_draw(SnxUiList* l, int sw, int sh, const int safe[4], int display_percent);
#endif
