/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc's captures (gpu/vk/renderdoc.c) against a stand-in RenderDoc, a case a run, its name the argument:
   none     neither renderdoc= nor RenderDoc's program: nothing is loaded, Ctrl+F12 captures nothing and says why, and
            the stats box has no RenderDoc line
   missing  renderdoc= set without the library: said with its name and why, nothing captured
   started  RenderDoc's program started the game (its library in the process), renderdoc= empty: its API taken and
            nothing loaded, the captures left where RenderDoc keeps them, its keys and overlay off (F12 is the stats');
            the stats box told a capture can be taken. A frame is captured only when asked: Ctrl+F12 starts the
            capture at the next present, after the frame's commands are recorded, and ends it at the one after, the
            stats box told the capture and its file; pressed twice before a present, one frame; a failed capture told
            as failed
   setting  renderdoc= set: the library loaded by its name, the captures at that stem; a frame renderdoc_frames names
            and Ctrl+F12's together, each once */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/unattended.h"
#include "renderdoc_app.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

VkState g_vk;

/* ---- the stand-in RenderDoc, and what the code under test did with it */
static struct {
    int loaded, installed;        /* its library in the process already; there to load */
    char looked[64], opened[64];  /* the names plat_dl_loaded and plat_dl_open were given */
    char env[128];                /* what plat_setenv set */
    char path[256];               /* the captures' path template */
    int path_set;                 /* SetCaptureFilePathTemplate's calls */
    int keys_num, keys_set;       /* SetCaptureKeys' */
    int overlay_and, overlay_set; /* MaskOverlayBits' */
    int end_ok;                   /* what EndFrameCapture answers */
    unsigned captures;            /* the captures written */
    char events[64];              /* s a recording sync, b a capture's start, e its end, in order */
    void* device;                 /* the device both were given */
    char log[4096];               /* every line logged */
    SnxStatsCapture shown;        /* the stats box's RenderDoc lines, as last told */
    int told;
    u64 ms;
    void* dispatch;          /* the instance's dispatch table, which RenderDoc's device pointer is */
    RENDERDOC_API_1_1_2 api; /* its functions, below */
} g;

static void event(char c) {
    size_t n = strlen(g.events);
    assert(n + 1 < sizeof g.events);
    g.events[n] = c;
}
static void RENDERDOC_CC set_path(const char* t) {
    snprintf(g.path, sizeof g.path, "%s", t);
    g.path_set++;
}
static const char* RENDERDOC_CC get_path(void) { return g.path; }
static void RENDERDOC_CC mask_overlay(uint32_t and_bits, uint32_t or_bits) {
    g.overlay_and = (int)and_bits;
    g.overlay_set += 1 + (int)or_bits;
}
static void RENDERDOC_CC capture_keys(RENDERDOC_InputButton* keys, int num) {
    g.keys_num = keys ? -1 : num;
    g.keys_set++;
}
static void RENDERDOC_CC start(RENDERDOC_DevicePointer dev, RENDERDOC_WindowHandle wnd) {
    (void)wnd;
    g.device = dev;
    event('b');
}
static uint32_t RENDERDOC_CC end(RENDERDOC_DevicePointer dev, RENDERDOC_WindowHandle wnd) {
    (void)wnd;
    assert(dev == g.device);
    event('e');
    if (g.end_ok) g.captures++;
    return (uint32_t)g.end_ok;
}
static uint32_t RENDERDOC_CC num_captures(void) { return g.captures; }
/* capture idx's file, as RenderDoc names one a program asked for: the template, _capture, then _2 and on */
static uint32_t RENDERDOC_CC get_capture(uint32_t idx, char* filename, uint32_t* len, uint64_t* when) {
    char path[300];
    if (idx >= g.captures) return 0;
    if (idx)
        snprintf(path, sizeof path, "%s_capture_%u.rdc", g.path, idx + 1);
    else
        snprintf(path, sizeof path, "%s_capture.rdc", g.path);
    if (filename) memcpy(filename, path, strlen(path) + 1);
    if (len) *len = (uint32_t)strlen(path) + 1;
    if (when) *when = 0;
    return 1;
}
static int RENDERDOC_CC get_api(RENDERDOC_Version version, void** out) {
    if (version != eRENDERDOC_API_Version_1_1_2) return 0;
    *out = &g.api;
    return 1;
}

/* ---- the platform and the renderer around renderdoc.c */
void* plat_dl_loaded(const char* name) {
    snprintf(g.looked, sizeof g.looked, "%s", name);
    return g.loaded ? &g.api : NULL;
}
void* plat_dl_open(const char* path) {
    snprintf(g.opened, sizeof g.opened, "%s", path);
    return g.installed || g.loaded ? &g.api : NULL;
}
void* plat_dl_sym(void* h, const char* name) {
    void* f;
    pRENDERDOC_GetAPI fn = get_api;
    assert(h == &g.api);
    if (strcmp(name, "RENDERDOC_GetAPI")) return NULL;
    memcpy(&f, &fn, sizeof f);
    return f;
}
const char* plat_dl_error(void) { return "no such file"; }
int plat_setenv(const char* name, const char* value) {
    snprintf(g.env, sizeof g.env, "%s=%s", name, value);
    return 0;
}
u64 unattended_ms(void) { return g.ms; }
void vk_rec_sync(void) { event('s'); }
void stats_capture(const SnxStatsCapture* c) {
    g.shown = *c;
    g.told++;
}
void logf_(int lvl, const char* fmt, ...) {
    size_t n = strlen(g.log);
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g.log + n, sizeof g.log - n, fmt, ap);
    va_end(ap);
}

/* the presents from..to, each ending its frame; the events they made, from none */
static const char* presents(u64 from, u64 to) {
    memset(g.events, 0, sizeof g.events);
    for (; from <= to; ++from) vk_renderdoc_present(from);
    return g.events;
}
static int logged(const char* line) { return strstr(g.log, line) != NULL; }

/* neither: nothing taken, and Ctrl+F12 says why */
static void case_none(void) {
    vk_renderdoc_load();
    assert(!strcmp(g.looked, SNX_VK_RENDERDOC_LIBRARY) && !g.opened[0] && !g.env[0] && !g.told);
    assert(!vk_renderdoc_capture(0) && !g.log[0]);
    assert(!vk_renderdoc_capture(1));
    assert(logged("renderdoc: Ctrl+F12 captures nothing: RenderDoc isn't in the process (renderdoc= is empty, and "
                  "RenderDoc's program didn't start the game)\n"));
    assert(!strcmp(presents(1, 5), "") && !g.told);
}
/* renderdoc= without the library */
static void case_missing(void) {
    snprintf(g_settings.render_doc_captures, sizeof g_settings.render_doc_captures, "/caps/game");
    vk_renderdoc_load();
    assert(!strcmp(g.opened, SNX_VK_RENDERDOC_LIBRARY) && !g.env[0] && !g.told);
    assert(logged("renderdoc: renderdoc=/caps/game is set but RenderDoc's library, " SNX_VK_RENDERDOC_LIBRARY
                  ", didn't load: no such file\n"));
    assert(!vk_renderdoc_capture(0) && !vk_renderdoc_capture(1) && !strcmp(presents(1, 5), ""));
}
/* RenderDoc's program started the game */
static void case_started(void) {
    g.loaded = 1;
    g.end_ok = 1;
    snprintf(g.path, sizeof g.path, "/tmp/RenderDoc/scarletnx_2026.11.07");
    vk_renderdoc_load();
    assert(!g.opened[0] && !g.path_set && !strcmp(g.path, "/tmp/RenderDoc/scarletnx_2026.11.07"));
    assert(g.keys_set == 1 && g.keys_num == 0 && g.overlay_set == 1 && g.overlay_and == eRENDERDOC_Overlay_None);
    assert(!strcmp(g.env, "ENABLE_VULKAN_RENDERDOC_CAPTURE=1"));
    assert(logged("renderdoc: in the process (RenderDoc's program started the game), Ctrl+F12 captures the next "
                  "frame, 0 frame(s) listed, to /tmp/RenderDoc/scarletnx_2026.11.07\n"));
    assert(g.told == 1 && g.shown.ready && !g.shown.taken && !g.shown.failed && !g.shown.path[0]);
    /* nothing asked: no capture */
    assert(vk_renderdoc_capture(0) && !strcmp(presents(1, 10), "") && g.told == 1);
    /* asked: started at the next present after its frame is recorded, ended at the one after, then nothing */
    assert(vk_renderdoc_capture(1) && logged("renderdoc: Ctrl+F12: the next frame is captured\n"));
    assert(!strcmp(presents(11, 11), "sb") && g.device == g.dispatch && g.told == 1);
    assert(!strcmp(presents(12, 12), "se") && g.captures == 1);
    assert(g.told == 2 && g.shown.ready && g.shown.taken == 1 && !g.shown.failed);
    assert(!strcmp(g.shown.path, "/tmp/RenderDoc/scarletnx_2026.11.07_capture.rdc"));
    assert(logged("renderdoc: captured present 12 to /tmp/RenderDoc/scarletnx_2026.11.07_capture.rdc\n"));
    assert(!strcmp(presents(13, 20), "") && g.captures == 1);
    /* pressed twice before a present: one frame */
    assert(vk_renderdoc_capture(1) && vk_renderdoc_capture(1));
    assert(!strcmp(presents(21, 25), "sbse") && g.captures == 2 && g.shown.taken == 2);
    assert(!strcmp(g.shown.path, "/tmp/RenderDoc/scarletnx_2026.11.07_capture_2.rdc"));
    /* a capture RenderDoc couldn't write: told as failed, with the captures written before it */
    g.end_ok = 0;
    assert(vk_renderdoc_capture(1) && !strcmp(presents(26, 28), "sbse"));
    assert(g.shown.ready && g.shown.failed && g.shown.taken == 2 && !g.shown.path[0]);
    assert(logged("renderdoc: the capture of present 27 failed\n"));
}
/* renderdoc= loads it; renderdoc_frames and Ctrl+F12 together */
static void case_setting(void) {
    g.installed = 1;
    g.end_ok = 1;
    snprintf(g_settings.render_doc_captures, sizeof g_settings.render_doc_captures, "/caps/game");
    snprintf(g_settings.render_doc_frames, sizeof g_settings.render_doc_frames, "30");
    vk_renderdoc_load();
    assert(!strcmp(g.looked, SNX_VK_RENDERDOC_LIBRARY) && !strcmp(g.opened, SNX_VK_RENDERDOC_LIBRARY));
    assert(g.path_set == 1 && !strcmp(g.path, "/caps/game") && g.keys_set == 1 && g.keys_num == 0);
    assert(logged("renderdoc: loaded, Ctrl+F12 captures the next frame, 1 frame(s) listed, to /caps/game\n"));
    assert(g.told == 1 && g.shown.ready);
    /* Ctrl+F12 for frame 11, and frame 30 as listed: each captured once */
    assert(vk_renderdoc_capture(1) && !strcmp(presents(10, 11), "sbse") && g.captures == 1);
    assert(!strcmp(g.shown.path, "/caps/game_capture.rdc") && g.shown.taken == 1);
    assert(!strcmp(presents(12, 40), "sbse") && g.captures == 2 && !strcmp(g.shown.path, "/caps/game_capture_2.rdc"));
    assert(logged("renderdoc: captured present 30 to /caps/game_capture_2.rdc\n"));
}

int main(int argc, char** argv) {
    g.api = (RENDERDOC_API_1_1_2){.SetCaptureFilePathTemplate = set_path,
                                  .GetCaptureFilePathTemplate = get_path,
                                  .MaskOverlayBits = mask_overlay,
                                  .SetCaptureKeys = capture_keys,
                                  .StartFrameCapture = start,
                                  .EndFrameCapture = end,
                                  .GetNumCaptures = num_captures,
                                  .GetCapture = get_capture};
    g.dispatch = &g;
    g_vk.instance = (VkInstance)(void*)&g.dispatch; /* RenderDoc's device pointer is the instance's first word */
    if (argc != 2) {
        fprintf(stderr, "usage: snx-test-vk-renderdoc none|missing|started|setting\n");
        return 2;
    }
    if (!strcmp(argv[1], "none"))
        case_none();
    else if (!strcmp(argv[1], "missing"))
        case_missing();
    else if (!strcmp(argv[1], "started"))
        case_started();
    else if (!strcmp(argv[1], "setting"))
        case_setting();
    else {
        fprintf(stderr, "%s is no case: none, missing, started or setting\n", argv[1]);
        return 2;
    }
    printf("vk_renderdoc %s passed\n", argv[1]);
    return 0;
}
