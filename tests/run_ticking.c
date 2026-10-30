/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_run_ticking (src/common/utilities/common.c): the tick comes about once a second while the child runs, its
   output captured or not, and the child's output and status are what snx_run gives */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "run_ticking.c:%d: check failed: %s\n", __LINE__, #e);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static void pause_ms(long ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = ms % 1000 * 1000000L;
    while (nanosleep(&t, &t)) {}
#endif
}
static void count(void* data) { ++*(int*)data; }

int main(int argc, char** argv) {
    const char *command[3], *quick[4];
    char *self, *output;
    int ticks, rc;
    double start, took;
    if (argc >= 2 && !strcmp(argv[1], "--child")) { /* a tool that says something, works a while, and fails */
        puts("before");
        fflush(stdout);
        if (argc == 2) pause_ms(1600); /* "--child now": no tick to wait for */
        puts("after");
        return 3;
    }
    self = snx_executable(argv[0]);
    CHECK(self != NULL);
    command[0] = self;
    command[1] = "--child";
    command[2] = NULL;
    /* captured: the output whole, the status, a tick a second of the child's time */
    ticks = 0;
    output = NULL;
    start = snx_seconds();
    rc = snx_run_ticking(command, NULL, NULL, &output, count, &ticks, 0, NULL);
    took = snx_seconds() - start;
    CHECK(rc == 3 && output && strstr(output, "before") && strstr(output, "after"));
    CHECK(ticks >= 1 && ticks <= (int)took);
    free(output);
    /* not captured: the child's end is waited for, ticking too */
    ticks = 0;
    rc = snx_run_ticking(command, NULL, NULL, NULL, count, &ticks, 0, NULL);
    CHECK(rc == 3 && ticks >= 1);
    /* snx_run, no tick */
    quick[0] = self;
    quick[1] = "--child";
    quick[2] = "now";
    quick[3] = NULL;
    output = NULL;
    CHECK(snx_run(quick, NULL, NULL, &output) == 3 && output && strstr(output, "after"));
    free(output);
    free(self);
    puts("run_ticking passed");
    return 0;
}
