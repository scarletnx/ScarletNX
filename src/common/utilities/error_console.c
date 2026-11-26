/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/error_console.h"
#include "common/utilities/game_log.h"
#include "common/utilities/common.h"
#include "common/utilities/error_sdl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the program's arguments as it was started: one allocation, the pointers then their bytes */
static struct {
    char** args;
} g_error_console;
void snx_error_args(int argc, const char* const* argv) {
    size_t bytes = (size_t)(argc + 1) * sizeof(char*);
    char* at;
    for (int i = 0; i < argc; ++i) bytes += strlen(argv[i]) + 1;
    free(g_error_console.args);
    g_error_console.args = malloc(bytes);
    if (!g_error_console.args) return;
    at = (char*)(g_error_console.args + argc + 1);
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(at, argv[i], n);
        g_error_console.args[i] = at;
        at += n;
    }
    g_error_console.args[argc] = NULL;
}
const char* const* snx_error_saved_args(void) { return (const char* const*)g_error_console.args; }

/* args started as a process of their own, not waited for: 0, or nonzero with SDL's reason in why */
static int start(const char* const* args, char* why, size_t why_size) {
    SDL_Process* p = SDL_CreateProcess(args, false);
    if (!p) {
        snprintf(why, why_size, "%s could not be started: %s", args[0], SDL_GetError());
        return 1;
    }
    SDL_DestroyProcess(p); /* it goes on: this one ends */
    return 0;
}

int snx_error_follow(const SnxError* e, int action, const char* program, const char* const* argv, char* why,
                     size_t why_size) {
    char line[160], value[1200], *open;
    if (action == SNX_ACTION_CLOSE || action <= SNX_ACTION_NONE) return 0;
    if (e->from_launcher) {
        snx_error_action_said(e, action, line, sizeof line);
        printf("%s\n", line);
        fflush(stdout);
        return 0;
    }
    if (!argv) argv = snx_error_saved_args();
    if (action == SNX_ACTION_TRY_AGAIN) {
        size_t n = 0, k = 0;
        int failed;
        while (argv && argv[n]) ++n;
        const char** args = calloc(n + 2, sizeof *args);
        if (!args) {
            snprintf(why, why_size, "out of memory to start %s again", program);
            return 1;
        }
        args[k++] = program;
        /* started by a driver's name (a link, or the multicall's first argument): that name first, as it was */
        if (n && !strncmp(snx_path_leaf(argv[0]), "scarletnx-", 10)) args[k++] = snx_path_leaf(argv[0]);
        for (size_t i = 1; i < n; ++i) args[k++] = argv[i];
        failed = start(args, why, why_size);
        free((void*)args);
        return failed;
    }
    snx_error_open_value(e, action, value, sizeof value);
    if (!value[0]) {
        snprintf(why, why_size, "the action %s is no page of the launcher", snx_error_action_name(action));
        return 1;
    }
    open = snx_format("--open=%s", value);
    if (!open) {
        snprintf(why, why_size, "out of memory to start the launcher at %s", value);
        return 1;
    }
    const char* args[] = {program, "scarletnx-switch", open, NULL};
    int failed = start(args, why, why_size);
    free(open);
    return failed;
}

int snx_error_console_end(int status, const char* const* argv) {
    const SnxError* noted = snx_error_noted();
    char why[1400], line[160], *self;
    SnxError e;
    int action;
    if (!status || !noted) return status;
    e = *noted;
    snx_game_log_sync(); /* the panel's Share Log takes the game log with the stop's own lines */
    if (!e.log[0] && snx_game_log_path()) snprintf(e.log, sizeof e.log, "%s", snx_game_log_path());
    self = snx_executable(NULL);
    if (self) snprintf(e.program, sizeof e.program, "%s", self); /* what Try Again runs again */
    action = snx_error_sdl_window(&e, NULL, why, sizeof why);
    if (action < 0) {
        fprintf(stderr, "error: %s\n", why);
        snx_error_said(&e, 0, line, sizeof line);
    } else
        snx_error_said(&e, 1, line, sizeof line);
    printf("%s\n", line);
    fflush(stdout);
    if (action >= 0 && self && snx_error_follow(&e, action, self, argv, why, sizeof why))
        fprintf(stderr, "error: %s\n", why);
    free(self);
    return status;
}
