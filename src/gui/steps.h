/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_STEPS_H
#define SNX_GUI_STEPS_H
#include <stddef.h>
#include <stdint.h>

/* A run as a loading screen tells it: the driver's and the runner's lines it knows become steps, each running
   until the next one starts, and the last ends with the process. */
#define GUI_STEPS_MAX 24
#define GUI_NOTES_MAX 128
typedef enum { GUI_STEP_ACTIVE, GUI_STEP_DONE, GUI_STEP_FAILED } GuiStepState;
typedef struct {
    char label[12], detail[96];
    GuiStepState state;
    uint32_t value, total; /* the recompile's blocks, or the MB an unpack or an extraction has written (megabytes) */
    int megabytes;
    uint64_t shown_ms;
    uint32_t serial;
} GuiStep;
/* a line printed during a step, for the detailed screen */
typedef struct {
    char text[112];
    uint64_t shown_ms;
    uint32_t step; /* its step's serial */
} GuiNote;
typedef struct {
    GuiStep step[GUI_STEPS_MAX]; /* the latest, the oldest dropped */
    int count;
    GuiNote note[GUI_NOTES_MAX]; /* the latest, the oldest dropped */
    int notes;
    uint32_t serial;
    char line[256];
    size_t length;
    int gpu;
    /* a program a collection's program started (the driver's "The game starts its program N, ..."): its PROGRAM step's
       serial until the program's window opens (the runtime's "video: a window of ..."), 0 for none; and N */
    uint32_t switching;
    int program;
} GuiSteps;
void gui_steps_reset(GuiSteps* steps);
void gui_steps_feed(GuiSteps* steps, const char* data, size_t size, uint64_t now_ms);
void gui_steps_finish(GuiSteps* steps, int ok);
/* how far the step at work is, 0 to 100: the unpack, the extraction or the recompile; 100 once the game launches; -1
   before anything says */
int gui_steps_percent(const GuiSteps* steps);
/* how a step's progress reads on its line, as a theme's [run] progress says */
typedef enum { GUI_PROGRESS_AMOUNT, GUI_PROGRESS_PERCENT, GUI_PROGRESS_TIME } GuiProgress;
/* step's progress into out as how says, empty while it has none: amount "3 / 6", "300 / 900 MB", "1.2 / 4.0 GB";
   percent "30%"; time what is left at the pace so far, as rEden's loading screen estimates it, "01:23 left", "--:--
   left" before a second of it */
void gui_step_progress(const GuiStep* step, int how, uint64_t now_ms, char* out, size_t size);
/* the latest step of label, NULL for none: LAUNCH, the runner's banner, says the game is loading */
const GuiStep* gui_steps_last(const GuiSteps* steps, const char* label);
/* the latest max notes of a step, oldest first, into out; their count */
int gui_step_notes(const GuiSteps* steps, const GuiStep* step, const GuiNote** out, int max);
#endif
