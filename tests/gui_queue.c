/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The queue of R's and B's recompiles (gui/queue.h): a job's argv kept whole until it starts, one of the same game and
   kind not added twice, its place in the line, a full queue and an argv too long refused, one with a game's 100 add-ons
   kept, a line taken out, how far the compiles going on have gone, and the places as the rows say them */
#include "gui/queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* job i's argv as it was given */
static void argv_is(const GuiQueue* q, int i, const char* const* want) {
    const char* got[GUI_QUEUE_ARGS];
    int n = gui_queue_argv(q, i, got), k;
    for (k = 0; want[k]; ++k) assert(k < n && !strcmp(got[k], want[k]));
    assert(k == n && !got[n]);
}
static void place_is(int place, const char* want) {
    char text[32];
    gui_queue_place_text(place, text, sizeof text);
    if (strcmp(text, want)) fprintf(stderr, "gui_queue: place %d reads %s, not %s\n", place, text, want);
    assert(!strcmp(text, want));
}
int main(void) {
    GuiQueue q = {0};
    const char* one[] = {"scarletnx", "-r", "/games/one/one", "--opt=2", "--data", "/data", NULL};
    const char* two[] = {"scarletnx", "-r", "/games/two/two", NULL};
    const char* many[GUI_QUEUE_ARGS + 1];
    bool added;
    int i;
    /* added at the end, waiting, its argv and names kept whole, a serial each */
    assert(gui_queue_add(&q, one, "/games/one/one", "games/one/one", "One", GUI_JOB_RECOMPILE, true, &added) == 0);
    assert(added && q.count == 1 && q.job[0].state == GUI_JOB_WAITING && q.job[0].percent == -1 && q.job[0].native);
    assert(!strcmp(gui_queue_file(&q, 0), "/games/one/one") && !strcmp(gui_queue_name(&q, 0), "One"));
    argv_is(&q, 0, one);
    assert(gui_queue_add(&q, two, "/games/two/two", "/games/two/two", "Two", GUI_JOB_RECOMPILE, false, &added) == 1);
    assert(added && q.job[1].serial == q.job[0].serial + 1);
    /* the same game and kind again: that one, nothing added; B's pack of it is a job of its own */
    assert(gui_queue_add(&q, two, "/games/two/two", "/games/two/two", "Two", GUI_JOB_RECOMPILE, false, &added) == 1);
    assert(!added && q.count == 2);
    assert(gui_queue_add(&q, two, "/games/two/two", "/games/two/two", "Two", GUI_JOB_AOT, false, &added) == 2);
    assert(added && q.count == 3 && q.job[2].kind == GUI_JOB_AOT);
    /* a game's compile, opened before it was made, and its recompile are one job: R on it while it waits adds none */
    assert(gui_queue_add(&q, two, "/games/two/two", "/games/two/two", "Two", GUI_JOB_COMPILE, false, &added) == 1);
    assert(!added && q.count == 3 && q.job[1].kind == GUI_JOB_RECOMPILE);
    /* found by its file or the path it is listed by, while it waits or runs */
    assert(gui_queue_find(&q, "/games/one/one") == 0 && gui_queue_find(&q, "games/one/one") == 0);
    assert(gui_queue_find(&q, "/games/three/three") == -1);
    /* places: the first waiting first while none runs; the running one first, then those waiting in their order */
    assert(gui_queue_running(&q) == -1 && gui_queue_next(&q) == 0 && gui_queue_waiting(&q) == 3);
    assert(gui_queue_place(&q, 0) == 1 && gui_queue_place(&q, 1) == 2 && gui_queue_place(&q, 2) == 3);
    q.job[0].state = GUI_JOB_RUNNING;
    assert(gui_queue_running(&q) == 0 && gui_queue_next(&q) == 1 && gui_queue_waiting(&q) == 2);
    assert(gui_queue_place(&q, 0) == 1 && gui_queue_place(&q, 1) == 2 && gui_queue_place(&q, 2) == 3);
    /* one that ended has no place, and is no longer found: its game plays */
    q.job[0].state = GUI_JOB_OK;
    q.job[1].state = GUI_JOB_RUNNING;
    assert(gui_queue_place(&q, 0) == 0 && gui_queue_place(&q, 1) == 1 && gui_queue_place(&q, 2) == 2);
    assert(gui_queue_find(&q, "/games/one/one") == -1);
    /* an ended one of the same game: a new job */
    assert(gui_queue_add(&q, one, "/games/one/one", "games/one/one", "One", GUI_JOB_RECOMPILE, true, &added) == 3);
    assert(added && gui_queue_place(&q, 3) == 3);
    /* taken out: those after it a place up, their texts their own */
    gui_queue_remove(&q, 1);
    assert(q.count == 3 && !strcmp(gui_queue_name(&q, 1), "Two") && q.job[1].kind == GUI_JOB_AOT);
    assert(!strcmp(gui_queue_name(&q, 2), "One") && gui_queue_running(&q) == -1 && gui_queue_place(&q, 1) == 1);
    argv_is(&q, 2, one);
    gui_queue_remove(&q, 7); /* none: nothing */
    assert(q.count == 3);
    /* full: refused, the queue as it was */
    for (i = q.count; i < GUI_QUEUE_MAX; ++i) {
        char file[32];
        snprintf(file, sizeof file, "/games/g%d/g%d", i, i);
        assert(gui_queue_add(&q, two, file, file, file, GUI_JOB_RECOMPILE, false, &added) == i && added);
    }
    assert(gui_queue_add(&q, two, "/games/x/x", "/games/x/x", "X", GUI_JOB_RECOMPILE, false, &added) == -1);
    assert(!added && q.count == GUI_QUEUE_MAX);
    gui_queue_free(&q);
    assert(!q.count && !q.job[0].text);
    /* an argv of GUI_QUEUE_ARGS - 1 strings fits with its NULL, one more does not */
    for (i = 0; i < GUI_QUEUE_ARGS; ++i) many[i] = "--opt=2";
    many[GUI_QUEUE_ARGS] = NULL;
    assert(gui_queue_add(&q, many, "/games/x/x", "/games/x/x", "X", GUI_JOB_RECOMPILE, false, &added) == -1);
    many[GUI_QUEUE_ARGS - 1] = NULL;
    assert(gui_queue_add(&q, many, "/games/x/x", "/games/x/x", "X", GUI_JOB_RECOMPILE, false, &added) == 0);
    gui_queue_free(&q);
    { /* a game's recompile with its update and 99 DLCs, --update and a dump each: kept whole, each its own */
        static char dumps[100][32];
        const char* hundred[3 + 2 * 100 + 2 + 1];
        int n = 0;
        hundred[n++] = "scarletnx";
        hundred[n++] = "-r";
        hundred[n++] = "/dumps/game.xci";
        for (i = 0; i < 100; ++i) {
            snprintf(dumps[i], sizeof dumps[i], "/dumps/add-on %d.nsp", i);
            hundred[n++] = "--update";
            hundred[n++] = dumps[i];
        }
        hundred[n++] = "--data";
        hundred[n++] = "/data";
        hundred[n] = NULL;
        assert(gui_queue_add(&q, hundred, "/dumps/game.xci", "/dumps/game.xci", "G", GUI_JOB_COMPILE, false, &added) ==
               0);
        argv_is(&q, 0, hundred);
        gui_queue_free(&q);
    }
    /* how far the compiles going on have gone: from the one added while none waited or ran, an ended one whole, the
       running one as far as it said, a waiting one at 0; the ended ones of the compiles before count for nothing */
    assert(gui_queue_add(&q, one, "/games/a/a", "/games/a/a", "A", GUI_JOB_RECOMPILE, false, &added) == 0);
    assert(gui_queue_percent(&q) == -1); /* none said yet */
    q.job[0].state = GUI_JOB_RUNNING;
    assert(gui_queue_percent(&q) == -1);
    q.job[0].percent = 40;
    assert(gui_queue_percent(&q) == 40); /* one game: its own */
    assert(gui_queue_add(&q, two, "/games/b/b", "/games/b/b", "B", GUI_JOB_RECOMPILE, false, &added) == 1);
    assert(gui_queue_add(&q, two, "/games/c/c", "/games/c/c", "C", GUI_JOB_AOT, false, &added) == 2);
    assert(gui_queue_percent(&q) == 13); /* 40 + 0 + 0 of three */
    q.job[0].state = GUI_JOB_OK;
    q.job[1].state = GUI_JOB_RUNNING;
    q.job[1].percent = 50;
    assert(gui_queue_percent(&q) == 50); /* 100 + 50 + 0 */
    q.job[1].state = GUI_JOB_FAILED;
    q.job[2].state = GUI_JOB_RUNNING; /* it has not said how far: 0 of it */
    assert(gui_queue_percent(&q) == 66);
    q.job[2].state = GUI_JOB_OK;
    assert(gui_queue_percent(&q) == 100);
    assert(gui_queue_add(&q, one, "/games/d/d", "/games/d/d", "D", GUI_JOB_RECOMPILE, false, &added) == 3);
    assert(gui_queue_percent(&q) == -1); /* compiles anew: the three that ended are not in it */
    q.job[3].state = GUI_JOB_RUNNING;
    q.job[3].percent = 30;
    assert(gui_queue_percent(&q) == 30);
    gui_queue_free(&q);
    /* the places as the rows say them: next, then English's ordinals, 11th to 13th apart */
    place_is(2, "next");
    place_is(3, "3rd");
    place_is(4, "4th");
    place_is(11, "11th");
    place_is(12, "12th");
    place_is(13, "13th");
    place_is(21, "21st");
    place_is(22, "22nd");
    place_is(23, "23rd");
    place_is(101, "101st");
    place_is(111, "111th");
    puts("GUI queue: added once, places, full, taken out, its percent and its words passed");
    return 0;
}
