/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc captures of frames. RenderDoc's library comes in by renderdoc=PREFIX ([Debug] or --renderdoc=), which loads
   it before the Vulkan instance exists (its capture layer is an implicit one, which the loader enables with
   ENABLE_VULKAN_RENDERDOC_CAPTURE), or is in the process already when RenderDoc's own program started the game. Either
   way the capture key (hotkey_capture, backend/sdl3.c) or the game's menu's Capture frame (overlay.c) captures a frame,
   the one after the next present, and renderdoc_frames=N,M,... names frames to capture: a number is a present, a number
   with s a time in seconds on the clock --script counts from (the first frame after it), which a loaded machine doesn't
   move. A capture holds one guest frame: everything recorded after present N-1 up to present N, written where
   renderdoc= says (PREFIX_capture.rdc, then PREFIX_capture_2.rdc and on), or where RenderDoc's program keeps its
   captures when it is empty. Frames that never reach the swapchain (offscreen, headless) are captured the same way,
   because the capture starts and ends here rather than at RenderDoc's present hook. RenderDoc writes a capture inside
   the present that ends it, the game stopped meanwhile (70 s for a frame of 1.4 GB): a notice over the game says so as
   a capture is asked or RenderDoc's program starts its own (IsFrameCapturing), a thread of ours logs how long it has
   taken every 5 s, and the log and a notice say how long it took, the file and its size once written (GetNumCaptures
   grown), or that it failed. RenderDoc's own keys (F12, Print Screen) and overlay are off, so F12 stays the stats' key:
   the stats box says when a capture can be taken, and what the last one wrote (stats_capture). The in-application API
   is RenderDoc's renderdoc_app.h (MIT, Baldur Karlsson), fetched by cmake/dependencies.cmake. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/unattended.h"

#if defined(__NetBSD__)
/* RenderDoc has no build for NetBSD, and renderdoc_app.h stops on a system it doesn't name */
void vk_renderdoc_load(void) {
    if (*g_settings.render_doc_captures)
        WARN("renderdoc: renderdoc=%s is set, but RenderDoc has no build for NetBSD: no frame is captured\n",
             g_settings.render_doc_captures);
}
int vk_renderdoc_capture(int ask) {
    if (ask) LOG("renderdoc: no frame is captured: RenderDoc has no build for NetBSD\n");
    return 0;
}
void vk_renderdoc_present(u64 present) { (void)present; }
#else
#include "renderdoc_app.h"

#define RD_FRAMES      64                     /* frames to capture at most */
#define RD_PROGRESS_NS (5ull * 1000000000ull) /* while RenderDoc writes a capture, how often the log says so */
/* over the game as a capture is asked or RenderDoc starts its own: RenderDoc writes it inside the present that ends
   it, the game stopped meanwhile (70 s for a frame of 1.4 GB). A notice's line, OVERLAY_COLS_MAX letters at most */
#define RD_CAPTURING   "RenderDoc: capturing; the game stops while it writes (a minute if large)"

typedef struct {
    u64 at;   /* the present, or the time in ms; 0 once taken */
    int secs; /* at is a time */
} RdFrame;
static struct {
    RENDERDOC_API_1_1_2* api; /* NULL: RenderDoc isn't in the process */
    RdFrame frames[RD_FRAMES];
    int nframes;
    int open;  /* a capture of ours runs, to end at the next present */
    int asked; /* the capture key's or the menu's: the main thread sets it, the next present arms it */
    /* asked a present ago: the capture starts at this one, so the frame shown while it is written was drawn after the
       ask, with RD_CAPTURING over it */
    int armed;
    int theirs;      /* a capture RenderDoc started itself (its program's), seen running at a present */
    unsigned before; /* the captures written when it was seen */
    /* the write in progress, which the progress thread reads: its number (0 for none, the writes started so far
       numbering them), and when it began (plat_ns) */
    PMutex lock;
    unsigned writing, writes;
    u64 since;
} g_renderdoc = {.lock = PMUTEX_INIT};

void vk_renderdoc_load(void) {
    const char *prefix = g_settings.render_doc_captures, *frames = g_settings.render_doc_frames, *p;
    pRENDERDOC_GetAPI get_api;
    /* RenderDoc's program started the game: its library is in the process, and renderdoc= needn't load it */
    void* lib = plat_dl_loaded(SNX_VK_RENDERDOC_LIBRARY);
    void* api = NULL; /* GetAPI stores a void*, which the API's pointer isn't */
    int started = lib != NULL;
    if (!lib && !*prefix) return;
    if (!lib) lib = plat_dl_open(SNX_VK_RENDERDOC_LIBRARY);
    if (!lib) {
        WARN("renderdoc: renderdoc=%s is set but RenderDoc's library, %s, didn't load: %s\n", prefix,
             SNX_VK_RENDERDOC_LIBRARY, plat_dl_error());
        return;
    }
    get_api = (pRENDERDOC_GetAPI)plat_dl_sym(lib, "RENDERDOC_GetAPI");
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2, &api) || !api) {
        WARN("renderdoc: %s has no in-application API 1.1.2: no frame is captured\n", SNX_VK_RENDERDOC_LIBRARY);
        return;
    }
    RENDERDOC_API_1_1_2* rd = api;
    /* the capture layer comes in through the loader, which only enables it with this set */
    if (plat_setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1")) WARN("renderdoc: couldn't enable the capture layer\n");
    if (*prefix) rd->SetCaptureFilePathTemplate(prefix);
    /* no overlay in the captured frames, and nothing taken but the presents asked for: F12 is the stats' key */
    rd->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    rd->SetCaptureKeys(NULL, 0);
    for (p = frames; *p && g_renderdoc.nframes < RD_FRAMES;) {
        RdFrame* f = &g_renderdoc.frames[g_renderdoc.nframes];
        char* e;
        unsigned long long n = strtoull(p, &e, 10);
        if (e == p) break;
        f->secs = *e == 's';
        f->at = *e == 's' ? n * 1000 : n;
        if (*e == 's') e++;
        if (f->at) g_renderdoc.nframes++;
        p = *e == ',' ? e + 1 : e;
    }
    if (*p)
        WARN("renderdoc: renderdoc_frames: \"%s\" is no frame (N, or Ns for seconds): it captures the %d "
             "before it\n",
             p, g_renderdoc.nframes);
    LOG("renderdoc: %s, %s%sthe menu's Capture frame captures a frame, %d frame(s) listed, to %s\n",
        started ? "in the process (RenderDoc's program started the game)" : "loaded",
        g_settings.render_doc_capture_shortcut, *g_settings.render_doc_capture_shortcut ? " or " : "",
        g_renderdoc.nframes, *prefix ? prefix : rd->GetCaptureFilePathTemplate());
    __atomic_store_n(&g_renderdoc.api, rd, __ATOMIC_RELEASE); /* the main thread's capture key reads it */
    stats_capture(&(SnxStatsCapture){.ready = 1});
}

int vk_renderdoc_capture(int ask) {
    if (!__atomic_load_n(&g_renderdoc.api, __ATOMIC_ACQUIRE)) {
        if (ask)
            LOG("renderdoc: no frame is captured: RenderDoc isn't in the process (renderdoc= is empty, and "
                "RenderDoc's program didn't start the game)\n");
        return 0;
    }
    if (ask) {
        __atomic_store_n(&g_renderdoc.asked, 1, __ATOMIC_RELEASE);
        overlay_notice(RD_CAPTURING);
        LOG("renderdoc: a capture is asked: the frame after the next present\n");
    }
    return 1;
}

/* the progress thread's: every RD_PROGRESS_NS while write gen is in progress, how long it has taken. It ends with it */
static void rd_progress(void* arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    int on;
    do {
        plat_sleep_ns((i64)RD_PROGRESS_NS);
        pm_lock(&g_renderdoc.lock);
        on = g_renderdoc.writing == gen;
        if (on)
            LOG("renderdoc: writing capture, %llu s\n",
                (unsigned long long)((plat_ns() - g_renderdoc.since) / 1000000000u));
        pm_unlock(&g_renderdoc.lock);
    } while (on);
}
/* a write begins, its progress thread with it */
static void rd_write_begin(void) {
    unsigned gen;
    pm_lock(&g_renderdoc.lock);
    gen = g_renderdoc.writing = ++g_renderdoc.writes;
    g_renderdoc.since = plat_ns();
    pm_unlock(&g_renderdoc.lock);
    if (plat_thread(rd_progress, (void*)(uintptr_t)gen, 0))
        WARN("renderdoc: no thread for the write's progress: the log says the capture once it is written\n");
}
/* the write ended: how long it took, in seconds */
static u64 rd_write_end(void) {
    u64 secs;
    pm_lock(&g_renderdoc.lock);
    g_renderdoc.writing = 0;
    secs = (plat_ns() - g_renderdoc.since) / 1000000000u;
    pm_unlock(&g_renderdoc.lock);
    return secs;
}
/* a file's size as the notices say it, ? where there is no file */
static void rd_size(const char* path, char* out, size_t n) {
    u64 b = snx_is_file(path) ? snx_file_size(path) : 0;
    if (!snx_is_file(path))
        snprintf(out, n, "?");
    else if (b < 1024)
        snprintf(out, n, "%llu bytes", (unsigned long long)b);
    else if (b < 1024 * 1024)
        snprintf(out, n, "%llu KiB", (unsigned long long)(b >> 10));
    else if (b < 1024ull * 1024 * 1024)
        snprintf(out, n, "%.1f MiB", (double)b / (1024.0 * 1024));
    else
        snprintf(out, n, "%.1f GiB", (double)b / (1024.0 * 1024 * 1024));
}
/* capture n (from 1) written in secs, what it holds (present N, or RenderDoc's own): its path and size in the log, its
   file's name and size over the game, a name too long for the notice's line keeping its end after "...", and the
   stats box told */
static void rd_written(RENDERDOC_API_1_1_2* rd, unsigned n, u64 secs, const char* what) {
    SnxStatsCapture shown = {.ready = 1};
    char size[32] = "?", lead[48], line[OVERLAY_COLS_MAX + 1];
    const char *name = "?", *c;
    size_t room, len;
    u32 path_len = 0;
    /* GetCapture writes the whole path, unbounded: its length first */
    int told = n && rd->GetCapture(n - 1, NULL, &path_len, NULL);
    shown.taken = n;
    if (told && path_len < sizeof shown.path && rd->GetCapture(n - 1, shown.path, NULL, NULL)) {
        rd_size(shown.path, size, sizeof size);
        for (name = c = shown.path; *c; ++c)
            if (*c == '/' || *c == '\\') name = c + 1;
        LOG("renderdoc: %s written in %llu s to %s (%s)\n", what, (unsigned long long)secs, shown.path, size);
    } else if (told) {
        shown.path[0] = 0;
        LOG("renderdoc: %s written in %llu s, capture %u, to a path of %u bytes, past the %zu kept\n", what,
            (unsigned long long)secs, n, path_len, sizeof shown.path);
    } else {
        shown.path[0] = 0;
        LOG("renderdoc: %s written in %llu s, capture %u, to a path RenderDoc doesn't give\n", what,
            (unsigned long long)secs, n);
    }
    snprintf(lead, sizeof lead, "RenderDoc: written in %llu s: ", (unsigned long long)secs);
    room = OVERLAY_COLS_MAX - strlen(lead) - strlen(size) - 3; /* " (" and ")" */
    len = strlen(name);
    if (len <= room)
        snprintf(line, sizeof line, "%s%s (%s)", lead, name, size);
    else
        snprintf(line, sizeof line, "%s...%s (%s)", lead, name + len - (room - 3), size);
    overlay_notice(line);
    stats_capture(&shown);
}
/* a capture that wrote no file after secs, what it held: in the log with what to read, over the game, the stats box
   told */
static void rd_failed(RENDERDOC_API_1_1_2* rd, u64 secs, const char* what) {
    char line[OVERLAY_COLS_MAX + 1];
    WARN("renderdoc: %s failed after %llu s: RenderDoc wrote no file (its own log says why)\n", what,
         (unsigned long long)secs);
    snprintf(line, sizeof line, "RenderDoc: capture failed after %llu s, no file written (see its log)",
             (unsigned long long)secs);
    overlay_notice(line);
    stats_capture(&(SnxStatsCapture){.ready = 1, .taken = rd->GetNumCaptures(), .failed = 1});
}
/* RenderDoc's own capture, seen at an earlier present, which it writes in the present that ends it: written once
   its count grew, failed once it stopped without a new one */
static void rd_theirs(RENDERDOC_API_1_1_2* rd) {
    unsigned n = rd->GetNumCaptures();
    if (n > g_renderdoc.before) {
        g_renderdoc.theirs = 0;
        rd_written(rd, n, rd_write_end(), "RenderDoc's own capture");
    } else if (!rd->IsFrameCapturing()) {
        g_renderdoc.theirs = 0;
        rd_failed(rd, rd_write_end(), "RenderDoc's own capture");
    }
}

/* whether the frame after this present is one to capture (each is taken once) */
static int rd_wanted(u64 present) {
    u64 ms = unattended_ms();
    int i, want = 0;
    for (i = 0; i < g_renderdoc.nframes; i++) {
        RdFrame* f = &g_renderdoc.frames[i];
        if (f->at && (f->secs ? ms >= f->at : f->at == present + 1)) {
            f->at = 0;
            want = 1;
        }
    }
    return want;
}
/* at the present that ends frame `present`, after its last submit and before the swapchain present, under vk_lock: the
   capture of this frame ends here, and the next frame's starts */
void vk_renderdoc_present(u64 present) {
    RENDERDOC_API_1_1_2* rd = __atomic_load_n(&g_renderdoc.api, __ATOMIC_ACQUIRE);
    void* dev;
    int want;
    if (!rd || !g_vk.instance) return;
    dev = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(g_vk.instance);
    if (g_renderdoc.theirs) rd_theirs(rd);
    want = rd_wanted(present) || g_renderdoc.armed; /* listed once: it takes the frame it answers for */
    g_renderdoc.armed = __atomic_exchange_n(&g_renderdoc.asked, 0, __ATOMIC_ACQ_REL);
    if (g_renderdoc.open || want) vk_rec_sync(); /* the frame's commands are recorded within the capture */
    if (g_renderdoc.open) {                      /* RenderDoc writes it here, the game stopped */
        char what[48];
        u64 secs;
        int ok;
        g_renderdoc.open = 0;
        snprintf(what, sizeof what, "the capture of present %llu", (unsigned long long)present);
        rd_write_begin();
        ok = (int)rd->EndFrameCapture(dev, NULL);
        secs = rd_write_end();
        if (ok)
            rd_written(rd, rd->GetNumCaptures(), secs, what);
        else
            rd_failed(rd, secs, what);
    } else if (!g_renderdoc.theirs && rd->IsFrameCapturing()) {
        /* RenderDoc's own, its program's: it writes it in the swapchain's present that ends the frame */
        g_renderdoc.theirs = 1;
        g_renderdoc.before = rd->GetNumCaptures();
        overlay_notice(RD_CAPTURING);
        LOG("renderdoc: RenderDoc's own capture runs, from present %llu: the game stops while it writes\n",
            (unsigned long long)present);
        rd_write_begin();
    }
    if (want) {
        rd->StartFrameCapture(dev, NULL);
        g_renderdoc.open = 1;
    }
}
#endif
