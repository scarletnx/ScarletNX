/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc's captures (gpu/vk/renderdoc.c) against a stand-in RenderDoc, a case a run, its name the argument:
   none     neither renderdoc= nor RenderDoc's program: nothing is loaded, the capture key captures nothing and says
            why, and the stats box has no RenderDoc line
   missing  renderdoc= set without the library: said with its name and why, nothing captured
   started  RenderDoc's program started the game (its library in the process), renderdoc= empty: its API taken and
            nothing loaded, the captures left where RenderDoc keeps them, its keys and overlay off (F12 is the stats');
            the stats box told a capture can be taken. A frame is captured only when asked: the ask said over the
            game, the capture started a present later (the frame shown while it is written drawn after the ask), after
            the frame's commands are recorded, and ended at the one after, the stats box told the capture and its
            file; asked twice before a present, one frame; a failed capture told as failed
   setting  renderdoc= set: the library loaded by its name, the captures at that stem; a frame renderdoc_frames names
            and the capture key's together, each once
   write    a capture's write, which stops the game: a thread of ours says it every 5 s, then the log and the notice
            over the game say how long it took, the file and its size (bytes to GiB, ? without a file); a name too
            long for the notice's line keeps its end
   theirs   a capture RenderDoc's program started itself: said over the game at the first present it runs at, written
            once RenderDoc's count of captures grew, failed once it stopped without */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/unattended.h"
#include "renderdoc_app.h"
#include "nx/runtime/platform/overlay.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
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
    int in_capture;          /* between StartFrameCapture and EndFrameCapture */
    int theirs;              /* a capture RenderDoc's program started runs */
    char notice[256];        /* overlay_notice's last line, and how many were given */
    int notices;
    u64 size; /* a capture file's size, when there is one (size_known) */
    int size_known;
    /* the clock (plat_ns), the progress thread's sleeps the test lets end (ticks), the threads asked for and whether
       they run; and what EndFrameCapture's write takes: that many 5 s ticks, each waited for its line, then tail_ns */
    u64 now;
    int ticks, threads, run_threads, write_ticks, ended; /* ended: the progress threads that returned */
    u64 tail_ns;
    void* pairs[8][2]; /* each progress thread's function and argument */
    pthread_mutex_t m; /* the log, the clock, the ticks and the notice, between the test and the progress thread */
    pthread_cond_t cv;
} g = {.m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
#define SECOND 1000000000ull

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
    g.in_capture = 1;
    event('b');
}
/* the progress lines logged so far */
static int progress_lines(void) {
    const char* p = g.log;
    int n = 0;
    while ((p = strstr(p, "renderdoc: writing capture, ")) != NULL) ++n, ++p;
    return n;
}
/* the write: write_ticks of 5 s, each let go to the progress thread and waited for until it logs its line, then
   tail_ns more */
static uint32_t RENDERDOC_CC end(RENDERDOC_DevicePointer dev, RENDERDOC_WindowHandle wnd) {
    int k;
    (void)wnd;
    assert(dev == g.device);
    event('e');
    g.in_capture = 0;
    pthread_mutex_lock(&g.m);
    for (k = 1; k <= g.write_ticks; ++k) {
        g.now += 5 * SECOND;
        g.ticks++;
        pthread_cond_broadcast(&g.cv);
        while (progress_lines() < k) pthread_cond_wait(&g.cv, &g.m);
    }
    g.now += g.tail_ns;
    pthread_mutex_unlock(&g.m);
    if (g.end_ok) g.captures++;
    return (uint32_t)g.end_ok;
}
static uint32_t RENDERDOC_CC num_captures(void) { return g.captures; }
static uint32_t RENDERDOC_CC is_capturing(void) { return (uint32_t)(g.in_capture || g.theirs); }
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
    size_t n;
    va_list ap;
    (void)lvl;
    pthread_mutex_lock(&g.m);
    n = strlen(g.log);
    va_start(ap, fmt);
    vsnprintf(g.log + n, sizeof g.log - n, fmt, ap);
    va_end(ap);
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.m);
}
void overlay_notice(const char* text) {
    pthread_mutex_lock(&g.m);
    snprintf(g.notice, sizeof g.notice, "%s", text);
    g.notices++;
    pthread_mutex_unlock(&g.m);
}
int snx_is_file(const char* path) { return strstr(path, "_capture") && g.size_known; }
SnxU64 snx_file_size(const char* path) { return snx_is_file(path) ? g.size : 0; }
u64 plat_ns(void) {
    u64 now;
    pthread_mutex_lock(&g.m);
    now = g.now;
    pthread_mutex_unlock(&g.m);
    return now;
}
/* the progress thread's sleep: until the test lets one tick go */
void plat_sleep_ns(i64 ns) {
    assert(ns == (i64)(5 * SECOND));
    pthread_mutex_lock(&g.m);
    while (!g.ticks) pthread_cond_wait(&g.cv, &g.m);
    g.ticks--;
    pthread_mutex_unlock(&g.m);
}
/* the progress thread, run where the case lets it */
static void* thread_run(void* arg) {
    void** pair = arg;
    PThreadFn fn;
    memcpy(&fn, &pair[0], sizeof fn);
    fn(pair[1]);
    pthread_mutex_lock(&g.m);
    g.ended++;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.m);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    (void)stack;
    assert(g.threads < 8);
    memcpy(&g.pairs[g.threads][0], &fn, sizeof fn);
    g.pairs[g.threads][1] = arg;
    if (g.run_threads) {
        assert(!pthread_create(&t, NULL, thread_run, g.pairs[g.threads]));
        pthread_detach(t);
    }
    g.threads++;
    return 0;
}
void pm_lock(PMutex* m) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&m->l);
#else
    pthread_mutex_lock(&m->l);
#endif
}
void pm_unlock(PMutex* m) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&m->l);
#else
    pthread_mutex_unlock(&m->l);
#endif
}

/* the presents from..to, each ending its frame; the events they made, from none */
static const char* presents(u64 from, u64 to) {
    memset(g.events, 0, sizeof g.events);
    for (; from <= to; ++from) vk_renderdoc_present(from);
    return g.events;
}
static int logged(const char* line) {
    int found;
    pthread_mutex_lock(&g.m);
    found = strstr(g.log, line) != NULL;
    pthread_mutex_unlock(&g.m);
    return found;
}
#define CAPTURING "RenderDoc: capturing; the game stops while it writes (a minute if large)"

/* neither: nothing taken, and the capture key says why */
static void case_none(void) {
    vk_renderdoc_load();
    assert(!strcmp(g.looked, SNX_VK_RENDERDOC_LIBRARY) && !g.opened[0] && !g.env[0] && !g.told);
    assert(!vk_renderdoc_capture(0) && !g.log[0]);
    assert(!vk_renderdoc_capture(1));
    assert(logged("renderdoc: no frame is captured: RenderDoc isn't in the process (renderdoc= is empty, and "
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
    /* the capture key as its setting has it */
    snprintf(g_settings.render_doc_capture_shortcut, sizeof g_settings.render_doc_capture_shortcut, "Pause");
    vk_renderdoc_load();
    assert(!g.opened[0] && !g.path_set && !strcmp(g.path, "/tmp/RenderDoc/scarletnx_2026.11.07"));
    assert(g.keys_set == 1 && g.keys_num == 0 && g.overlay_set == 1 && g.overlay_and == eRENDERDOC_Overlay_None);
    assert(!strcmp(g.env, "ENABLE_VULKAN_RENDERDOC_CAPTURE=1"));
    assert(logged("renderdoc: in the process (RenderDoc's program started the game), Pause or the menu's Capture "
                  "frame captures a frame, 0 frame(s) listed, to /tmp/RenderDoc/scarletnx_2026.11.07\n"));
    assert(g.told == 1 && g.shown.ready && !g.shown.taken && !g.shown.failed && !g.shown.path[0]);
    /* nothing asked: no capture, no notice */
    assert(vk_renderdoc_capture(0) && !strcmp(presents(1, 10), "") && g.told == 1 && !g.notices);
    /* asked: said over the game, started a present later after its frame is recorded, ended at the one after, then
       nothing */
    assert(vk_renderdoc_capture(1) && logged("renderdoc: a capture is asked: the frame after the next present\n"));
    assert(g.notices == 1 && !strcmp(g.notice, CAPTURING) && strlen(CAPTURING) <= OVERLAY_COLS_MAX);
    assert(!strcmp(presents(11, 11), "") && !strcmp(presents(12, 12), "sb") && g.device == g.dispatch);
    assert(g.told == 1 && !strcmp(presents(13, 13), "se") && g.captures == 1);
    assert(g.told == 2 && g.shown.ready && g.shown.taken == 1 && !g.shown.failed);
    assert(!strcmp(g.shown.path, "/tmp/RenderDoc/scarletnx_2026.11.07_capture.rdc"));
    assert(logged("renderdoc: the capture of present 13 written in 0 s to "
                  "/tmp/RenderDoc/scarletnx_2026.11.07_capture.rdc (?)\n"));
    assert(g.notices == 2 && !strcmp(g.notice, "RenderDoc: written in 0 s: scarletnx_2026.11.07_capture.rdc (?)"));
    assert(!strcmp(presents(14, 20), "") && g.captures == 1 && g.notices == 2);
    /* pressed twice before a present: one frame */
    assert(vk_renderdoc_capture(1) && vk_renderdoc_capture(1));
    assert(!strcmp(presents(21, 25), "sbse") && g.captures == 2 && g.shown.taken == 2);
    assert(!strcmp(g.shown.path, "/tmp/RenderDoc/scarletnx_2026.11.07_capture_2.rdc"));
    /* a capture RenderDoc couldn't write: told as failed, with the captures written before it, and what to read */
    g.end_ok = 0;
    assert(vk_renderdoc_capture(1) && !strcmp(presents(26, 28), "sbse"));
    assert(g.shown.ready && g.shown.failed && g.shown.taken == 2 && !g.shown.path[0]);
    assert(logged("renderdoc: the capture of present 28 failed after 0 s: RenderDoc wrote no file (its own log says "
                  "why)\n"));
    assert(!strcmp(g.notice, "RenderDoc: capture failed after 0 s, no file written (see its log)"));
}
/* renderdoc= loads it; renderdoc_frames and the capture key together */
static void case_setting(void) {
    g.installed = 1;
    g.end_ok = 1;
    snprintf(g_settings.render_doc_captures, sizeof g_settings.render_doc_captures, "/caps/game");
    snprintf(g_settings.render_doc_frames, sizeof g_settings.render_doc_frames, "30");
    g_settings.render_doc_capture_shortcut[0] = 0; /* no capture key: the menu's item */
    vk_renderdoc_load();
    assert(!strcmp(g.looked, SNX_VK_RENDERDOC_LIBRARY) && !strcmp(g.opened, SNX_VK_RENDERDOC_LIBRARY));
    assert(g.path_set == 1 && !strcmp(g.path, "/caps/game") && g.keys_set == 1 && g.keys_num == 0);
    assert(logged("renderdoc: loaded, the menu's Capture frame captures a frame, 1 frame(s) listed, to "
                  "/caps/game\n"));
    assert(g.told == 1 && g.shown.ready);
    /* asked before present 10, so frame 12; and frame 30 as listed: each captured once */
    assert(vk_renderdoc_capture(1) && !strcmp(presents(10, 12), "sbse") && g.captures == 1);
    assert(!strcmp(g.shown.path, "/caps/game_capture.rdc") && g.shown.taken == 1);
    assert(!strcmp(presents(13, 40), "sbse") && g.captures == 2 && !strcmp(g.shown.path, "/caps/game_capture_2.rdc"));
    assert(logged("renderdoc: the capture of present 30 written in 0 s to /caps/game_capture_2.rdc (?)\n"));
}
/* a capture's write: a line every 5 s from a thread of ours while RenderDoc writes, then the time, the file and its
   size; each size as the notices say it; a name too long for the notice's line keeps its end */
static void case_write(void) {
    static const struct {
        u64 size;
        int known;
        const char* said;
    } SIZES[] = {{500, 1, "500 bytes"}, {2048, 1, "2 KiB"}, {7u << 19, 1, "3.5 MiB"}, {0, 0, "?"}};
    char name[300], line[OVERLAY_COLS_MAX + 2];
    size_t i;
    u64 at = 4;
    g.loaded = 1;
    g.end_ok = 1;
    g.run_threads = 1;
    g.now = 100 * SECOND; /* the seconds said from the write's start */
    snprintf(g.path, sizeof g.path, "/caps/game");
    vk_renderdoc_load();
    g.size = 1536ull << 20;
    g.size_known = 1;
    assert(vk_renderdoc_capture(1) && !strcmp(g.notice, CAPTURING));
    g.write_ticks = 2;
    g.tail_ns = 3 * SECOND;
    assert(!strcmp(presents(1, 2), "sb") && !strcmp(presents(3, 3), "se") && g.threads == 1);
    assert(progress_lines() == 2 && logged("renderdoc: writing capture, 5 s\nrenderdoc: writing capture, 10 s\n"));
    assert(logged("renderdoc: writing capture, 10 s\nrenderdoc: the capture of present 3 written in 13 s to "
                  "/caps/game_capture.rdc (1.5 GiB)\n"));
    assert(!strcmp(g.notice, "RenderDoc: written in 13 s: game_capture.rdc (1.5 GiB)"));
    /* written: the thread's next tick ends it, with no line */
    pthread_mutex_lock(&g.m);
    g.ticks++;
    pthread_cond_broadcast(&g.cv);
    while (!g.ended && progress_lines() == 2) pthread_cond_wait(&g.cv, &g.m);
    assert(g.ended == 1 && progress_lines() == 2);
    pthread_mutex_unlock(&g.m);
    g.write_ticks = 0;
    g.tail_ns = 0;
    for (i = 0; i < sizeof SIZES / sizeof *SIZES; ++i, at += 3) {
        g.size = SIZES[i].size;
        g.size_known = SIZES[i].known;
        vk_renderdoc_capture(1);
        presents(at, at + 2);
        snprintf(line, sizeof line, "RenderDoc: written in 0 s: game_capture_%u.rdc (%s)", (unsigned)i + 2,
                 SIZES[i].said);
        assert(!strcmp(g.notice, line));
    }
    /* a name the line doesn't hold: "..." and its end, the line full */
    snprintf(g.path, sizeof g.path, "/caps/%s", "a_stem_much_longer_than_the_line_of_a_notice_could_ever_hold");
    g.size = 2048;
    g.size_known = 1;
    vk_renderdoc_capture(1);
    presents(at, at + 2);
    snprintf(name, sizeof name, "%s_capture_%u.rdc", g.path, g.captures);
    assert(logged(name) && strlen(g.notice) == OVERLAY_COLS_MAX &&
           !strncmp(g.notice, "RenderDoc: written in 0 s: ...", 30));
    snprintf(line, sizeof line, "_capture_%u.rdc (2 KiB)", g.captures);
    assert(!strcmp(g.notice + OVERLAY_COLS_MAX - strlen(line), line));
}
/* RenderDoc's program's own capture: said at the first present it runs at, its write followed from there; written
   once its count grew; one that stops with none written failed */
static void case_theirs(void) {
    g.loaded = 1;
    snprintf(g.path, sizeof g.path, "/tmp/rd/game");
    vk_renderdoc_load();
    g.size = 7u << 19;
    g.size_known = 1;
    g.theirs = 1;
    assert(!strcmp(presents(1, 1), "") && g.notices == 1 && !strcmp(g.notice, CAPTURING) && g.threads == 1);
    assert(logged("renderdoc: RenderDoc's own capture runs, from present 1: the game stops while it writes\n"));
    /* written in the present that ended it: its count grew */
    g.theirs = 0;
    g.captures = 1;
    g.now += 2 * SECOND;
    assert(!strcmp(presents(2, 2), "") && g.shown.taken == 1 && !g.shown.failed);
    assert(logged("renderdoc: RenderDoc's own capture written in 2 s to /tmp/rd/game_capture.rdc (3.5 MiB)\n"));
    assert(!strcmp(g.notice, "RenderDoc: written in 2 s: game_capture.rdc (3.5 MiB)") && g.notices == 2);
    assert(!strcmp(presents(3, 5), "") && g.notices == 2 && g.threads == 1);
    /* one that ran over two presents and stopped with none written */
    g.theirs = 1;
    presents(6, 7);
    assert(g.notices == 3 && g.threads == 2);
    g.theirs = 0;
    g.now += SECOND;
    presents(8, 8);
    assert(g.shown.failed && g.shown.taken == 1);
    assert(logged("renderdoc: RenderDoc's own capture failed after 1 s: RenderDoc wrote no file (its own log says "
                  "why)\n"));
    assert(!strcmp(g.notice, "RenderDoc: capture failed after 1 s, no file written (see its log)"));
}

int main(int argc, char** argv) {
    g.api = (RENDERDOC_API_1_1_2){.SetCaptureFilePathTemplate = set_path,
                                  .GetCaptureFilePathTemplate = get_path,
                                  .MaskOverlayBits = mask_overlay,
                                  .SetCaptureKeys = capture_keys,
                                  .StartFrameCapture = start,
                                  .EndFrameCapture = end,
                                  .GetNumCaptures = num_captures,
                                  .GetCapture = get_capture,
                                  .IsFrameCapturing = is_capturing};
    g.dispatch = &g;
    g_vk.instance = (VkInstance)(void*)&g.dispatch; /* RenderDoc's device pointer is the instance's first word */
    if (argc != 2) {
        fprintf(stderr, "usage: snx-test-vk-renderdoc none|missing|started|setting|write|theirs\n");
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
    else if (!strcmp(argv[1], "write"))
        case_write();
    else if (!strcmp(argv[1], "theirs"))
        case_theirs();
    else {
        fprintf(stderr, "%s is no case: none, missing, started, setting, write or theirs\n", argv[1]);
        return 2;
    }
    printf("vk_renderdoc %s passed\n", argv[1]);
    return 0;
}
