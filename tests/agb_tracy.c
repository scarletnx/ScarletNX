/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's Tracy capture is not empty (common/utilities/tracy.c, the GBA's present in src/agb/runtime/vk.c): F10's
   capture (snx_tracy_capture_now), started over a GBA window under SDL's offscreen driver, records its presents; the
   trace, read back by tracy-csvexport, has the frames and the "agb present" zones of the frames drawn. A build with
   SNX_TRACY; skipped (77) without Tracy's tools (the arguments: tracy-capture, tracy-csvexport) or a Vulkan window */
#include "agb/ppu/ppu.h"
#include "agb/runtime/vk.h"
#include "common/utilities/common.h"
#include "common/utilities/tracy.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 120

static uint32_t g_frame[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT];

/* the capture tracy.c wrote in dir's captures/, owned; NULL for none */
static char* capture_in(const char* dir) {
    char *captures = snx_path_join(dir, "captures"), **names = NULL, *found = NULL;
    size_t count = 0;
    if (captures && !snx_list_dir(captures, &names, &count))
        for (size_t i = 0; i < count && !found; ++i)
            if (strstr(names[i], ".tracy")) found = snx_path_join(captures, names[i]);
    snx_free_strings(names, count);
    free(captures);
    return found;
}
/* tracy-csvexport's rows of trace (with -F, the frame sets'), into out: 0, or -1 */
static int export_rows(const char* tool, const char* trace, int frames, char* out, size_t size) {
    char* command = snx_format("\"%s\" %s\"%s\"", tool, frames ? "-F " : "", trace);
    FILE* p = command ? popen(command, "r") : NULL;
    size_t n = p ? fread(out, 1, size - 1, p) : 0;
    out[n] = 0;
    int status = p ? pclose(p) : -1;
    free(command);
    return p && !status ? 0 : -1;
}
/* the number in column column (from 0) of the row named name in csvexport's rows: the zones' (name,src_file,src_line,
   total_ns,total_perc,counts,...) or the frame sets' (frame_set,continuous,frames,...); -1 for no such row */
static long row_number(const char* rows, const char* name, int column) {
    for (const char* line = rows; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        size_t n = strlen(name);
        if (strncmp(line, name, n) || line[n] != ',') continue;
        const char* field = line;
        for (int i = 0; i < column && field; ++i) field = strchr(field + 1, ',');
        return field ? strtol(field + 1, NULL, 10) : -1;
    }
    return -1;
}

int main(int argc, char** argv) {
    if (argc != 3 || !snx_is_file(argv[1]) || !snx_is_file(argv[2])) {
        printf("agb_tracy: no tracy-capture and tracy-csvexport given (%s, %s): skipped\n", argc > 1 ? argv[1] : "-",
               argc > 2 ? argv[2] : "-");
        return 77;
    }
    char error[256] = {0}, rows[1 << 16];
    for (size_t i = 0; i < sizeof g_frame / sizeof *g_frame; ++i) g_frame[i] = 0xFF000000u | (uint32_t)(i * 40503u);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "agb_tracy: SDL's offscreen video: %s\n", SDL_GetError());
        return 1;
    }
    AgbVk* vk = agb_vk_open("agb_tracy", AGB_SCREEN_WIDTH * 2, AGB_SCREEN_HEIGHT * 2, error, sizeof error);
    if (!vk) {
        printf("agb_tracy: no Vulkan window here (%s): skipped\n", error);
        SDL_Quit();
        return 77;
    }
    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    if (!dir) return 1;
    /* the game's start with nothing asked, as a console's runner makes it; then F10 */
    snprintf(g_settings.tracy_capture_tool, sizeof g_settings.tracy_capture_tool, "%s", argv[1]);
    g_settings.tracy_capture_seconds = 30; /* longer than the run: the client's end closes it */
    g_settings.tracy_open_trace = 0;
    int failed = snx_tracy_console(dir, "agb_tracy", 1) != 0;
    snx_tracy_capture_now();
    for (int k = 0; k < FRAMES && !failed; ++k)
        if (!agb_vk_frame(vk, g_frame, &(SnxNotice){0})) { /* no notice over it */
            fprintf(stderr, "agb_tracy: frame %d did not go on screen\n", k);
            failed = 1;
        }
    snx_tracy_shutdown(); /* the client's end: the capture writes the trace */
    agb_vk_close(vk);
    SDL_Quit();
    char* trace = capture_in(dir);
    long presents = -1, frames = -1;
    if (!trace)
        fprintf(stderr, "agb_tracy: no capture in %s/captures\n", dir);
    else if (export_rows(argv[2], trace, 0, rows, sizeof rows))
        fprintf(stderr, "agb_tracy: tracy-csvexport could not read %s\n", trace);
    else {
        presents = row_number(rows, "agb present", 5);
        if (!export_rows(argv[2], trace, 1, rows, sizeof rows)) frames = row_number(rows, "Frame", 2);
        printf("agb_tracy: %s: %ld frames and %ld \"agb present\" zones of the %d frames drawn\n", trace, frames,
               presents, FRAMES);
    }
    /* every present after the capture connected is a zone and a frame mark, and the client marks a frame or two of
       its own at its start and end */
    if (presents <= 0 || presents > FRAMES || frames < presents) {
        fprintf(stderr, "agb_tracy: the capture %s has %ld frames and %ld present zones of %d presents\n",
                trace ? trace : "(none)", frames, presents, FRAMES);
        failed = 1;
    }
    if (!failed) (void)snx_remove_tree(dir);
    free(trace), free(dir), free(root);
    puts(failed ? "agb_tracy: failed" : "agb_tracy: passed");
    return failed;
}
