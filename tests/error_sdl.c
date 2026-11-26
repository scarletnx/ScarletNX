/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel in an SDL window (common/utilities/error_sdl.c) on SDL's dummy video and software renderer: the
   keys and a pad's buttons after the panel's wait take the action in focus or close it, any other key does nothing, a
   press before the wait does nothing, the window's close is Close; the panel's own process (--error-screen) exits with
   the action taken and refuses a record it cannot read; and a game's process starts it and gets that action back
   (error_spawn.c). Its presses come from a thread, as a person's come after the panel showed.

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
    Uint32 delay_ms;
    SDL_Keycode keys[8];
    int n, quit; /* quit: then the window's close */
} Presses;

static int press(void* data) {
    const Presses* p = data;
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
    SDL_Thread* t = SDL_CreateThread(press, "presses", (void*)p);
    int action = snx_error_sdl_window(e, NULL, why, sizeof why);
    SDL_WaitThread(t, NULL);
    if (action < 0) fprintf(stderr, "error_sdl: %s\n", why);
    return action;
}

int main(int argc, char** argv) {
    SnxError e = {.kind = SNX_ERROR_GPU_LOST, .game = "A Game", .program = "/g/G", .message = "vulkan: lost"};
    char path[1100], why[1400];
    if (argc > 1 && !strncmp(argv[1], SNX_ERROR_SCREEN, sizeof SNX_ERROR_SCREEN - 1)) {
        /* the panel's own process: Down and Enter, its second action */
        static Presses later = {.delay_ms = 700, .keys = {SDLK_DOWN, SDLK_RETURN}, .n = 2};
        SDL_Thread* t;
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        t = SDL_CreateThread(press, "presses", &later);
        int status = snx_error_screen_main(argc, argv);
        SDL_WaitThread(t, NULL);
        return status;
    }
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
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
        Presses p = {.delay_ms = 10, .keys = {SDLK_RETURN}, .n = 1},
                q = {.delay_ms = 800, .keys = {SDLK_RETURN}, .n = 1};
        SDL_Thread* t = SDL_CreateThread(press, "early", &p);
        char why2[400];
        SDL_Thread* u = SDL_CreateThread(press, "later", &q);
        int action = snx_error_sdl_window(&e, NULL, why2, sizeof why2);
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
        for (int k = 0; k < 100 && !snx_is_file(mark); ++k) SDL_Delay(20);
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
