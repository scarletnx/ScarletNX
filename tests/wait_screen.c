/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screens the player waits on decide (runtime/platform/wait_screen.c), on a clock of the test's: the game
   closing's screen from SNX_WAIT_CLOSING_MS into the save and only while the window is seen, the first close stopping
   the save and no later one, the line the log says then with the seconds and the MiB not saved, the row with the MiB
   packed and the seconds spent; a save state's box from SNX_WAIT_LOADING_MS into its load */
#include "nx/runtime/platform/wait_screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define MIB 1048576ull

static void closing(void) {
    SnxClosing w = {.began_ms = 20000};
    /* nothing to show before half a second, nor past it while the window is not seen (minimized, in the background) */
    CHECK(!snx_closing_pass(&w, 20000, 0, 1) && !w.shown);
    CHECK(!snx_closing_pass(&w, 20499, 0, 1) && !w.shown);
    CHECK(!snx_closing_pass(&w, 20500, 0, 0) && !w.shown);
    CHECK(!snx_closing_pass(&w, 20500, 0, 1) && w.shown);
    CHECK(!snx_closing_pass(&w, 21000, 0, 0) && !w.shown);
    CHECK(!snx_closing_pass(&w, 21000, 0, 1) && w.shown && !w.stopped_ms);
    /* a clock behind the start (another core's) is no time spent */
    {
        SnxClosing early = {.began_ms = 20000};
        CHECK(!snx_closing_pass(&early, 19000, 0, 1) && !early.shown);
    }
    /* the first close stops it, at once and before the screen shows too; a second does nothing more */
    CHECK(snx_closing_pass(&w, 22400, 1, 1) && w.stopped_ms == 22400 && w.shown);
    CHECK(!snx_closing_pass(&w, 22500, 2, 1) && w.stopped_ms == 22400);
    {
        SnxClosing quick = {.began_ms = 20000};
        CHECK(snx_closing_pass(&quick, 20100, 1, 1) && quick.stopped_ms == 20100 && !quick.shown);
        SnxClosing at_once = {.began_ms = 20000}; /* a close on the start's own millisecond still marks a stop */
        CHECK(snx_closing_pass(&at_once, 20000, 1, 1) && at_once.stopped_ms);
    }
    /* the log's line: the seconds into the save, the MiB of the session's left out and of all, the values as given */
    {
        char said[256];
        const u64 total = 166408602, kept = total - 96 * MIB; /* 158.7 MiB, of which 96.0 not saved */
        snx_closing_said(&w, total, kept, said, sizeof said);
        CHECK(!strcmp(said, "a second close stopped the shader cache's save 2.4 s in: 96.0 of 158.7 MiB of this "
                            "session's shaders not saved"));
        snx_closing_said(&w, 3 * MIB, 3 * MIB, said, sizeof said); /* all written before the stop took */
        CHECK(!strcmp(said, "a second close stopped the shader cache's save 2.4 s in: 0.0 of 3.0 MiB of this "
                            "session's shaders not saved"));
        snx_closing_said(&w, 3 * MIB, 0, said, sizeof said); /* none */
        CHECK(strstr(said, ": 3.0 of 3.0 MiB of this session's shaders not saved"));
    }
}
static void closing_row(void) {
    SnxClosing w = {.began_ms = 20000};
    SnxPrepareStep st;
    snx_closing_step(&w, 166408602, 96 * MIB, &st);
    CHECK(!strcmp(st.label, "SAVING SHADERS") && !strcmp(st.detail, "96.0 of 158.7 MiB"));
    CHECK(st.kib && st.value == 96 * 1024 && st.total == 166408602 / 1024 && !st.done && st.started_ms == 20000);
    /* all of it packed: done; more said packed than in all (a count read between two stores) as all of it */
    snx_closing_step(&w, 166408602, 166408602, &st);
    CHECK(st.done && st.value == st.total && !strcmp(st.detail, "158.7 of 158.7 MiB"));
    snx_closing_step(&w, 10 * MIB, 11 * MIB, &st);
    CHECK(st.done && st.value == 10 * 1024 && !strcmp(st.detail, "10.0 of 10.0 MiB"));
    /* before the save says its bytes: 0 of 0, no progress and not done */
    snx_closing_step(&w, 0, 0, &st);
    CHECK(!st.done && !st.total && !strcmp(st.detail, "0.0 of 0.0 MiB"));
}
static void loading(void) {
    CHECK(!snx_loading_shows(1000, 1000) && !snx_loading_shows(1000, 1299));
    CHECK(snx_loading_shows(1000, 1300) && snx_loading_shows(1000, 60000));
    CHECK(!snx_loading_shows(1000, 900)); /* a clock behind the start */
}
int main(void) {
    closing();
    closing_row();
    loading();
    printf("wait_screen: %u checks passed\n", checks);
    return 0;
}
