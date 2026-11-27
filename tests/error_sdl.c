/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel in an SDL window (common/utilities/error_sdl.c) on SDL's dummy video and software renderer: the
   keys and a pad's buttons after the panel's wait take the action in focus or close it, any other key does nothing, a
   press before the wait does nothing, the window's close is Close; the panel's own process (--error-screen) exits with
   the action taken and refuses a record it cannot read; and a game's process starts it and gets that action back
   (error_spawn.c). Its presses come from a thread, timed from when the panel showed (its line, or in its own process
   its window), as a person's come: not from the thread's start, which a loaded machine leaves far behind.

    tests/error_sdl [--error-screen=RECORD]   (the second form is the panel's own process, which this test starts) */
#include "common/utilities/common.h"
#include "common/utilities/error_console.h"
#include "common/utilities/error_sdl.h"
#include "common/utilities/error_spawn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    Uint32 delay_ms; /* after the panel showed */
    SDL_Keycode keys[8];
    int n, quit; /* quit: then the window's close */
    int own;     /* in the panel's own process: from its window, which shows before its panel starts */
} Presses;
static SDL_AtomicInt g_shown; /* the panel said it shows (said) */
static void said(const char* line) {
    fprintf(stderr, "%s\n", line);
    if (strstr(line, "the panel shows")) SDL_SetAtomicInt(&g_shown, 1);
}
static SDL_AtomicInt g_returned; /* the refused panel's call came back */
/* the window's close 3 s on, unless the call came back first: what ends a panel that waited where it should not */
static int close_later(void* data) {
    (void)data;
    for (int i = 0; i < 300 && !SDL_GetAtomicInt(&g_returned); ++i) SDL_Delay(10);
    if (!SDL_GetAtomicInt(&g_returned)) SDL_PushEvent(&(SDL_Event){.type = SDL_EVENT_QUIT});
    return 0;
}
static const SnxErrorSdlHooks g_hooks = {.say = said, .unseen = 1}; /* SDL's dummy video: this test's own presses */

static int press(void* data) {
    const Presses* p = data;
    int windows = 0;
    if (p->own) { /* its window, then room for the renderer it makes before the panel starts */
        for (SDL_Window** w; !windows; SDL_Delay(10)) SDL_free(w = SDL_GetWindows(&windows));
        SDL_Delay(1000);
    } else
        while (!SDL_GetAtomicInt(&g_shown)) SDL_Delay(5);
    SDL_Delay(p->delay_ms);
    for (int i = 0; i < p->n; ++i) {
        SDL_Event e = {0};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = p->keys[i];
        e.key.down = true;
        SDL_PushEvent(&e);
        SDL_Delay(30);
    }
    if (p->quit) SDL_PushEvent(&(SDL_Event){.type = SDL_EVENT_QUIT});
    return 0;
}
/* the panel in a window of its own, the presses given coming from a thread: the action it ended on */
static int run(SnxError* e, const Presses* p) {
    char why[400];
    SDL_SetAtomicInt(&g_shown, 0);
    SDL_Thread* t = SDL_CreateThread(press, "presses", (void*)p);
    int action = snx_error_sdl_window(e, &g_hooks, why, sizeof why);
    SDL_WaitThread(t, NULL);
    if (action < 0) fprintf(stderr, "error_sdl: %s\n", why);
    return action;
}

int main(int argc, char** argv) {
    SnxError e = {.kind = SNX_ERROR_GPU_LOST, .game = "A Game", .program = "/g/G", .message = "vulkan: lost"};
    char path[1100], why[1400];
    if (argc > 1 && !strncmp(argv[1], SNX_ERROR_SCREEN, sizeof SNX_ERROR_SCREEN - 1)) {
        /* the panel's own process: Down and Enter, its second action */
        static Presses later = {.delay_ms = 700, .keys = {SDLK_DOWN, SDLK_RETURN}, .n = 2, .own = 1};
        SDL_Thread* t;
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        t = SDL_CreateThread(press, "presses", &later);
        int status = snx_error_screen_run(argc, argv, &g_hooks);
        SDL_WaitThread(t, NULL);
        return status;
    }
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
    /* on a video driver that shows nothing no panel waits, unless a test drives it (unseen): -1, with why; an Esc a
       moment later would close one that waited */
    {
        SDL_Thread* t = SDL_CreateThread(close_later, "safety", NULL);
        int action = snx_error_sdl_window(&e, NULL, why, sizeof why);
        SDL_SetAtomicInt(&g_returned, 1);
        SDL_WaitThread(t, NULL);
        assert(action == -1 && strstr(why, "SDL's dummy video driver shows nothing"));
    }
    /* after the wait: Down, then Enter takes the second action (Try Again, GPU Drivers, ...) */
    {
        Presses p = {.delay_ms = 700, .keys = {SDLK_DOWN, SDLK_RETURN}, .n = 2};
        assert(run(&e, &p) == SNX_ACTION_GPU_DRIVERS);
    }
    /* a key of no use does nothing; Esc closes */
    {
        Presses p = {.delay_ms = 700, .keys = {SDLK_X, SDLK_ESCAPE}, .n = 2};
        assert(run(&e, &p) == SNX_ACTION_CLOSE);
    }
    /* an Enter in the wait (a press meant for the game) does nothing; the next, after it, does the first action */
    {
        Presses p = {.delay_ms = 0, .keys = {SDLK_RETURN}, .n = 1},
                q = {.delay_ms = 800, .keys = {SDLK_RETURN}, .n = 1};
        SDL_SetAtomicInt(&g_shown, 0);
        SDL_Thread* t = SDL_CreateThread(press, "early", &p);
        char why2[400];
        SDL_Thread* u = SDL_CreateThread(press, "later", &q);
        int action = snx_error_sdl_window(&e, &g_hooks, why2, sizeof why2);
        SDL_WaitThread(t, NULL);
        SDL_WaitThread(u, NULL);
        assert(action == SNX_ACTION_TRY_AGAIN);
    }
    /* Details and back keep the panel (Details' own items: Copy Text, Back), back on Details' item */
    {
        Presses p = {.delay_ms = 700,
                     .keys = {SDLK_UP, SDLK_UP, SDLK_RETURN, SDLK_DOWN, SDLK_RETURN, SDLK_UP, SDLK_RETURN},
                     .n = 7};
        assert(run(&e, &p) == SNX_ACTION_GPU_DRIVERS);
    }
    /* the window's close is Close */
    {
        Presses p = {.delay_ms = 700, .quit = 1};
        assert(run(&e, &p) == SNX_ACTION_CLOSE);
    }
    SDL_Quit();
    /* the panel's own process: a record it cannot read, a second argument; then this test as it, through error_spawn */
    {
        char* bad[] = {"t", "--error-screen=/nonexistent/record", NULL};
        char* two[] = {"t", "--error-screen=x", "y", NULL};
        assert(snx_error_screen_main(2, bad) == 2 && snx_error_screen_main(3, two) == 2);
        snprintf(path, sizeof path, "%s.record", argv[0]);
        assert(!snx_error_spawn_prepare(argv[0], path, why, sizeof why));
        assert(snx_error_spawn_ready());
        int action = snx_error_spawn(&e, why, sizeof why);
        if (action < 0) fprintf(stderr, "error_sdl: %s\n", why);
        assert(action == SNX_ACTION_GPU_DRIVERS);
        SnxError back;
        assert(!snx_error_read(&back, path, why, sizeof why) && back.kind == SNX_ERROR_GPU_LOST &&
               !strcmp(back.program, "/g/G"));
        remove(path);
        /* a program that does not show it: its status said */
        assert(!snx_error_spawn_prepare("/nonexistent/scarletnx", path, why, sizeof why));
        assert(snx_error_spawn(&e, why, sizeof why) == -1 && strstr(why, "showed nothing: its status 127"));
        remove(path);
    }
    /* what follows an action: for the launcher, the run's line on stdout; with none, Try Again starts the program again
       with the arguments the run had, after its first */
    {
        SnxError f = {.kind = SNX_ERROR_CRASH, .from_launcher = 1};
        char out[1200], said[200] = "", *mark;
        FILE* o;
        snprintf(out, sizeof out, "%s.follow", argv[0]);
        int kept = dup(1);
        fflush(stdout);
        o = freopen(out, "w", stdout);
        assert(o && !snx_error_follow(&f, SNX_ACTION_TRY_AGAIN, "/nonexistent", NULL, why, sizeof why));
        fflush(stdout);
        dup2(kept, 1); /* stdout as it was */
        close(kept);
        FILE* r = fopen(out, "r");
        assert(r && fgets(said, sizeof said, r));
        fclose(r);
        assert(!strcmp(said, "error: action try-again\n"));
        f.from_launcher = 0;
        mark = snx_format("%s.again", argv[0]);
        remove(mark);
        char* cmd = snx_format("echo again > '%s'", mark);
        const char* args[] = {"x", "-c", cmd, NULL};
        assert(!snx_error_follow(&f, SNX_ACTION_TRY_AGAIN, "/bin/sh", args, why, sizeof why));
        for (int k = 0; k < 1000 && !snx_is_file(mark); ++k) SDL_Delay(20); /* 20 s, for a loaded machine */
        assert(snx_is_file(mark));
        remove(mark);
        remove(out);
        free(cmd);
        free(mark);
        assert(snx_error_follow(&f, SNX_ACTION_SHARE_LOG, "/bin/sh", args, why, sizeof why) &&
               strstr(why, "share-log is no page of the launcher"));
    }
    puts("error_sdl: ok");
    return 0;
}
