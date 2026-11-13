/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A run's output as a loading screen's steps. */
#include "gui/steps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(GuiSteps* s, const char* text, uint64_t now) { gui_steps_feed(s, text, strlen(text), now); }
static const GuiStep* at(const GuiSteps* s, int i) { return &s->step[i]; }

int main(void) {
    GuiSteps s;
    int n, i;
    const GuiNote* notes[8];
    gui_steps_reset(&s);
    assert(gui_steps_percent(&s) == -1);
    feed(&s, "Extracting the update\nRecompiling Tool (1 modules)\r\nExtracting the base ", 10);
    assert(s.count == 2 && !strcmp(at(&s, 0)->label, "EXTRACT") && !strcmp(at(&s, 0)->detail, "the update"));
    assert(at(&s, 0)->state == GUI_STEP_DONE && at(&s, 1)->state == GUI_STEP_ACTIVE && at(&s, 1)->shown_ms == 10);
    feed(&s, "game\nan unknown line\nRecompiling Game (1 modules)\nFunctions: 9  instructions: 99\n", 20);
    assert(s.count == 4 && !strcmp(at(&s, 2)->label, "EXTRACT") && !strcmp(at(&s, 2)->detail, "the base game"));
    assert(!strcmp(at(&s, 3)->label, "RECOMPILE") && !strcmp(at(&s, 3)->detail, "Game"));
    assert(gui_steps_percent(&s) == 0);
    feed(&s, "  compiled block 1/4: code_000 (1.0 s)\n", 30);
    assert(at(&s, 3)->value == 1 && at(&s, 3)->total == 4 && gui_steps_percent(&s) == 25);
    feed(&s, "  compiled block 3/4: code_002 (1.0 s)\nLLVM module: 1.1 MB\nBuilt /games/Game\n", 40);
    assert(s.count == 5 && at(&s, 3)->state == GUI_STEP_DONE && !strcmp(at(&s, 4)->label, "BUILD"));
    assert(at(&s, 4)->state == GUI_STEP_DONE && gui_steps_percent(&s) == 100);
    feed(&s, "ScarletNX - Game\nvulkan: AMD Radeon RX 6700 XT (RADV NAVI22), Vulkan 1.4.354, driver 1\n", 50);
    feed(&s, "vulkan: pipeline cache loaded (662 KB)\nvulkan: Other GPU, Vulkan 1.3\n", 60);
    assert(s.count == 7 && !strcmp(at(&s, 5)->label, "LAUNCH") && !strcmp(at(&s, 5)->detail, "Game"));
    assert(!strcmp(at(&s, 6)->label, "GPU") && !strcmp(at(&s, 6)->detail, "AMD Radeon RX 6700 XT"));
    assert(at(&s, 6)->state == GUI_STEP_ACTIVE);
    gui_steps_finish(&s, 1);
    assert(at(&s, 6)->state == GUI_STEP_DONE);

    /* the lines of a step that start none: its notes, trimmed, the latest max of them, oldest first */
    n = gui_step_notes(&s, at(&s, 3), notes, 8);
    assert(n == 4 && !strcmp(notes[0]->text, "Functions: 9  instructions: 99"));
    assert(!strcmp(notes[1]->text, "compiled block 1/4: code_000 (1.0 s)") &&
           !strcmp(notes[3]->text, "LLVM module: 1.1 MB"));
    assert(notes[0]->shown_ms == 20 && notes[1]->shown_ms == 30);
    n = gui_step_notes(&s, at(&s, 3), notes, 2);
    assert(n == 2 && !strcmp(notes[0]->text, "compiled block 3/4: code_002 (1.0 s)"));
    assert(gui_step_notes(&s, at(&s, 2), notes, 8) == 1 && !strcmp(notes[0]->text, "an unknown line"));
    n = gui_step_notes(&s, at(&s, 6), notes, 8); /* the second device line is the GPU step's too */
    assert(n == 2 && !strcmp(notes[0]->text, "vulkan: pipeline cache loaded (662 KB)"));
    assert(gui_step_notes(&s, at(&s, 0), notes, 8) == 0 && gui_step_notes(&s, at(&s, 4), notes, 8) == 0);

    /* a collection's program that starts another: a PROGRAM step, the switch under way until the program's window
       opens (its recompile in between when it is not made), and nothing once it has */
    gui_steps_reset(&s);
    feed(&s, "ScarletNX - Game\nvideo: a window of 1280x720 pixels, the display's scale 1.00\n", 1);
    assert(s.switching == 0);
    feed(&s, "The game starts its program 2, 0100000000010000_0100000000010002 (ExecuteProgram from program 0)\n", 2);
    assert(s.count == 2 && !strcmp(at(&s, 1)->label, "PROGRAM") && !strcmp(at(&s, 1)->detail, "2"));
    assert(s.switching == at(&s, 1)->serial && s.program == 2 && at(&s, 0)->state == GUI_STEP_DONE);
    assert(s.previous == 0 && !strcmp(s.next, "0100000000010000_0100000000010002"));
    feed(&s, "Recompiling Game, program 2 of 3 (4 modules)\n  compiled block 1/4: code_000 (1.0 s)\n", 3);
    assert(s.switching == at(&s, 1)->serial && gui_steps_percent(&s) == 25);
    feed(&s, "ScarletNX - Game\nvideo: a window of 1280x720 pixels, the display's scale 1.00\n", 4);
    assert(s.switching == 0 && s.program == 2 && !strcmp(at(&s, 3)->label, "LAUNCH"));
    feed(&s, "The game starts its program x, of no number\n", 5); /* not the driver's line: a note */
    assert(s.switching == 0 && s.count == 4);
    feed(&s, "The game starts its program 1, Other\n", 6); /* no program it started from: not known */
    assert(s.program == 1 && s.previous == -1 && !strcmp(s.next, "Other"));

    /* Vita's compiler reports completed parallel chunks, not NX function blocks. */
    gui_steps_reset(&s);
    feed(&s, "Recompiling PCSG00001 (Vita AArch32)\nDiscovered 1200 blocks (4 addresses were not code)\n", 70);
    assert(gui_steps_last(&s, "RECOMPILE") && gui_steps_percent(&s) == 0);
    feed(&s, "Compiled 1/12 chunks of blocks\n", 80);
    assert(gui_steps_percent(&s) == 8);
    feed(&s, "Compiled 6/12 chunks of blocks\n", 90);
    assert(gui_steps_percent(&s) == 50);
    feed(&s, "Compiled 12/12 chunks of blocks\n", 100);
    assert(gui_steps_percent(&s) == 100 && at(&s, 0)->value == 12 && at(&s, 0)->total == 12);

    gui_steps_reset(&s);
    feed(&s, "a line before any step\n", 1);
    assert(s.notes == 0);
    feed(&s, "Recompiling Many (1 modules)\n", 1);
    for (i = 0; i < GUI_NOTES_MAX + 5; ++i) {
        char note_line[32];
        snprintf(note_line, sizeof note_line, "note %d\n", i);
        feed(&s, note_line, 2);
    }
    n = gui_step_notes(&s, at(&s, 0), notes, 8);
    assert(s.notes == GUI_NOTES_MAX && n == 8 && !strcmp(notes[7]->text, "note 132") &&
           !strcmp(s.note[0].text, "note 5"));

    /* a driver with no "(...)" after the device, and one further on the line */
    gui_steps_reset(&s);
    feed(&s, "vulkan: NVIDIA GeForce GT 1030, Vulkan 1.4.312, driver 912B4080, compute 1 (separate family)\n", 1);
    assert(s.count == 1 && !strcmp(at(&s, 0)->detail, "NVIDIA GeForce GT 1030"));

    /* a failed process fails the step it was in, even one already done, and a line cut short still counts */
    gui_steps_reset(&s);
    feed(&s, "Recompiling Broken (2 modules)\n  compiled block 1/2: x\nScarletNX - Bro", 5);
    gui_steps_finish(&s, 0);
    assert(s.count == 2 && at(&s, 0)->state == GUI_STEP_DONE && at(&s, 1)->state == GUI_STEP_FAILED);
    assert(!strcmp(at(&s, 1)->detail, "Bro") && gui_steps_percent(&s) == 100);
    gui_steps_reset(&s); /* a recompile that fails stops the bar where it was */
    feed(&s, "Recompiling Broken (1 modules)\n  compiled block 4/9: x\n", 5);
    gui_steps_finish(&s, 0);
    assert(at(&s, 0)->state == GUI_STEP_FAILED && gui_steps_percent(&s) == 44);
    gui_steps_reset(&s);
    feed(&s, "Built /games/Game\n", 5);
    gui_steps_finish(&s, 0);
    assert(at(&s, 0)->state == GUI_STEP_FAILED);

    /* on Android the driver ends naming the game it made ready, which the app then runs */
    gui_steps_reset(&s);
    feed(&s, "Recompiling Game (1 modules)\n  compiled block 1/4: x\nReady Game\n", 5);
    gui_steps_finish(&s, 1);
    assert(s.count == 2 && !strcmp(at(&s, 1)->label, "READY") && !strcmp(at(&s, 1)->detail, "Game"));
    assert(at(&s, 1)->state == GUI_STEP_DONE && gui_steps_percent(&s) == 100);

    /* the driver's row a second while hactool extracts, each over the last (\r): the step's value and total in MB, its
       percent the bar's, no note under it; a row of another step's word is a plain line */
    gui_steps_reset(&s);
    feed(&s, "Reading Game.xci\nExtracting the base game\n", 1);
    assert(s.count == 1 && gui_steps_percent(&s) == -1); /* nothing said yet */
    feed(&s, "  extracted 1024 of 4096 MB (25%)\r  extracted 2048 of 4096 MB (50%)\r", 2);
    assert(at(&s, 0)->value == 2048 && at(&s, 0)->total == 4096 && at(&s, 0)->megabytes);
    assert(gui_steps_percent(&s) == 50 && gui_step_notes(&s, at(&s, 0), notes, 8) == 0);
    feed(&s, "  extracted 4000 of 4096 MB (97%)\r\nRecompiling Game (1 modules)\n", 3);
    assert(at(&s, 0)->state == GUI_STEP_DONE && at(&s, 0)->value == 4000 && gui_steps_percent(&s) == 0);
    feed(&s, "  extracted 9 of 9 MB (100%)\n  compiled block 3/4: code_002 (1.0 s)\n", 4);
    assert(at(&s, 1)->value == 3 && at(&s, 1)->total == 4 && gui_steps_percent(&s) == 75);
    assert(gui_step_notes(&s, at(&s, 1), notes, 8) == 2 && !strcmp(notes[0]->text, "extracted 9 of 9 MB (100%)"));

    /* a step's progress on its line, as the theme says: amount in MB, GB past 1024 of them, blocks bare; percent;
       the time left at its pace so far, once a second of it has gone; nothing while it has no total */
    {
        GuiStep step;
        char text[32];
        memset(&step, 0, sizeof step);
        step.shown_ms = 1000;
        gui_step_progress(&step, GUI_PROGRESS_AMOUNT, 2000, text, sizeof text);
        assert(!strcmp(text, ""));
        step.value = 300;
        step.total = 900;
        step.megabytes = 1;
        gui_step_progress(&step, GUI_PROGRESS_AMOUNT, 2000, text, sizeof text);
        assert(!strcmp(text, "300 / 900 MB"));
        step.value = 2048;
        step.total = 4096;
        gui_step_progress(&step, GUI_PROGRESS_AMOUNT, 2000, text, sizeof text);
        assert(!strcmp(text, "2.0 / 4.0 GB"));
        gui_step_progress(&step, GUI_PROGRESS_PERCENT, 2000, text, sizeof text);
        assert(!strcmp(text, "50%"));
        gui_step_progress(&step, GUI_PROGRESS_TIME, 1500, text, sizeof text);
        assert(!strcmp(text, "--:-- left"));
        gui_step_progress(&step, GUI_PROGRESS_TIME, 31000, text, sizeof text); /* half in 30 s: 30 s more */
        assert(!strcmp(text, "00:30 left"));
        step.value = 1;
        step.total = 1000;
        gui_step_progress(&step, GUI_PROGRESS_TIME, 11000, text, sizeof text); /* 10 s a block, 999 to go */
        assert(!strcmp(text, "2:46:30 left"));
        step.megabytes = 0;
        step.value = 3;
        step.total = 6;
        gui_step_progress(&step, GUI_PROGRESS_AMOUNT, 2000, text, sizeof text);
        assert(!strcmp(text, "3 / 6"));
    }

    /* a native launch, no recompile: 100% from the banner; the oldest steps go past the maximum */
    gui_steps_reset(&s);
    feed(&s, "ScarletNX - Native\n", 1);
    assert(gui_steps_percent(&s) == 100);
    for (i = 0; i < GUI_STEPS_MAX + 3; ++i) {
        char line[64];
        snprintf(line, sizeof line, "Extracting part%d\nnote of part%d\n", i, i);
        feed(&s, line, (uint64_t)i);
    }
    assert(s.count == GUI_STEPS_MAX && !strcmp(at(&s, GUI_STEPS_MAX - 1)->detail, "part26"));
    assert(!strcmp(at(&s, 0)->detail, "part3"));
    /* each step still has its own lines once older steps are gone */
    n = gui_step_notes(&s, at(&s, GUI_STEPS_MAX - 1), notes, 8);
    assert(n == 1 && !strcmp(notes[0]->text, "note of part26"));
    n = gui_step_notes(&s, at(&s, 0), notes, 8);
    assert(n == 1 && !strcmp(notes[0]->text, "note of part3"));

    /* the runner's banner, at the start of a line, split across reads or not: the game is loading */
    gui_steps_reset(&s);
    feed(&s, "Recompiling x\nnot ScarletNX - x\nScarletNX", 1);
    assert(!gui_steps_last(&s, "LAUNCH") && gui_steps_last(&s, "RECOMPILE") == at(&s, 0));
    feed(&s, " - x\n", 2);
    assert(gui_steps_last(&s, "LAUNCH") == at(&s, 1) && !strcmp(at(&s, 1)->detail, "x"));
    gui_steps_reset(&s);
    feed(&s, "ScarletNX -x\n\nScarletNX  - x", 1);
    feed(&s, "\n", 1);
    assert(!gui_steps_last(&s, "LAUNCH"));
    feed(&s, "\n\nScarletNX - y\nScarletNX - z\n", 2);
    assert(gui_steps_last(&s, "LAUNCH") == at(&s, 1) && !strcmp(at(&s, 1)->detail, "z") &&
           !gui_steps_last(&s, "READY"));
    gui_steps_reset(&s); /* a banner cut short counts once the run ends */
    feed(&s, "ScarletNX - first line", 1);
    assert(!gui_steps_last(&s, "LAUNCH"));
    gui_steps_finish(&s, 1);
    assert(gui_steps_last(&s, "LAUNCH"));
    puts("GUI steps: driver and runner lines, progress, finish passed");
    return 0;
}
