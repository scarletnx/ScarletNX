/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's pause (runtime/pause.c's game_pause_toggle, hotkey_pause's), over faked guest threads, GPU, host1x and
   clocks: each press says the pause now and before it, the same words in the log and over the game; a pause whose
   threads or GPU don't stop in time is said with the game going on, and the next press tries again; and with another
   pause in force (a save state's page) the press is the player's own, which the other outlasts */
#include "nx/runtime/pause.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/video/host1x.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/unattended.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* what the pause's parts answer (0, or -1: not stopped in time) and how many holds each has, and the log's last line */
static struct {
    int kernel_fails, gpu_fails;
    int kernel_held, gpu_held, audio_held, clock_held, video_held;
    char logged[256];
} g_test;

int kernel_hold(int timeout_ms) {
    (void)timeout_ms;
    if (g_test.kernel_fails) return -1;
    g_test.kernel_held++;
    return 0;
}
void kernel_release(void) { g_test.kernel_held--; }
int host1x_drain(int timeout_ms) {
    (void)timeout_ms;
    return 0;
}
int gpu_hold(int timeout_ms) {
    (void)timeout_ms;
    if (g_test.gpu_fails) return -1;
    g_test.gpu_held++;
    return 0;
}
void gpu_release(void) { g_test.gpu_held--; }
void audio_hold(void) { g_test.audio_held++; }
void audio_release(void) { g_test.audio_held--; }
void native_video_hold(void) { g_test.video_held++; }
void native_video_release(void) { g_test.video_held--; }
void clock_pause(void) { g_test.clock_held++; }
void clock_resume(void) { g_test.clock_held--; }
u64 guest_ns(void) { return 0; }
u64 plat_ns(void) { return 0; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.logged, sizeof g_test.logged, fmt, ap);
    va_end(ap);
}
/* the rest pause.c calls, which these tests never reach: the unattended pauses' memory hash */
u8* g_mem;
MemLayout g_layout;
void mem_query(u64 addr, MemBlock* out) {
    (void)addr;
    memset(out, 0, sizeof *out);
}
int own_hash(u64 addr, u64 size, u64* h) {
    (void)addr;
    (void)size;
    (void)h;
    return 0;
}
u64 unattended_ms(void) { return 0; }

/* every part held once while paused, none while the game runs */
static int held(int n) {
    return g_test.kernel_held == n && g_test.gpu_held == n && g_test.audio_held == n && g_test.clock_held == n &&
           g_test.video_held == n;
}
/* a press: what it returns, its log line, and its words over the game: the log's up to " (was", the pause now alone */
static void press(int want, const char* line) {
    char text[96], logged[100], over[96];
    CHECK(game_pause_toggle(text, sizeof text) == want);
    snprintf(logged, sizeof logged, "%s\n", line);
    snprintf(over, sizeof over, "%.*s",
             (int)(strstr(line, " (was") ? strstr(line, " (was") - line : (long)strlen(line)), line);
    if (strcmp(text, over) || strcmp(g_test.logged, logged)) {
        fprintf(stderr, "pause: \"%s\", logged \"%s\"; expected \"%s\" and \"%s\"\n", text, g_test.logged, over, line);
        exit(1);
    }
}

int main(void) {
    char text[8];
    /* on, and off */
    press(1, "Pause: on (was off)");
    CHECK(game_paused() && held(1));
    press(0, "Pause: off (was on)");
    CHECK(!game_paused() && held(0));
    /* the guest threads, then the GPU, not stopped in time: the game goes on, said, and nothing stays held */
    g_test.kernel_fails = 1;
    press(-1, "Not paused: the game didn't stop within 2 s, the log says which part");
    CHECK(!game_paused() && held(0));
    g_test.kernel_fails = 0;
    g_test.gpu_fails = 1;
    press(-1, "Not paused: the game didn't stop within 2 s, the log says which part");
    CHECK(!game_paused() && held(0));
    g_test.gpu_fails = 0;
    /* the next press pauses: a pause that didn't happen is no pause to resume */
    press(1, "Pause: on (was off)");
    CHECK(game_paused() && held(1));
    press(0, "Pause: off (was on)");
    CHECK(!game_paused() && held(0));
    /* a save state's page holds the game: the press pauses with it, and its resume leaves the page's */
    CHECK(!game_pause() && held(1));
    press(1, "Pause: on (was off)");
    press(0, "Pause: off (was on)");
    CHECK(game_paused() && held(1));
    game_resume();
    CHECK(!game_paused() && held(0));
    /* a toast's buffer too small: cut, ended; no room given: the log's line alone */
    CHECK(game_pause_toggle(text, sizeof text) == 1 && !strcmp(text, "Pause: "));
    CHECK(game_pause_toggle(NULL, 0) == 0 && !strcmp(g_test.logged, "Pause: off (was on)\n"));
    printf("pause_toggle: ok\n");
    return 0;
}
