/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc captures of frames. RenderDoc's library comes in by renderdoc=PREFIX ([Debug] or --renderdoc=), which loads
   it before the Vulkan instance exists (its capture layer is an implicit one, which the loader enables with
   ENABLE_VULKAN_RENDERDOC_CAPTURE), or is in the process already when RenderDoc's own program started the game. Either
   way Ctrl+F12 (backend/sdl3.c) captures the next frame, and renderdoc_frames=N,M,... names frames to capture: a number
   is a present, a number with s a time in seconds on the clock --script counts from (the first frame after it), which
   a loaded machine doesn't move. A capture holds one guest frame: everything recorded after present N-1 up to present
   N, written where renderdoc= says (PREFIX_capture.rdc, then PREFIX_capture_2.rdc and on), or where RenderDoc's
   program keeps its captures when it is empty. Frames that never reach the swapchain (offscreen, headless) are
   captured the same way, because the capture starts and ends here rather than at RenderDoc's present hook. RenderDoc's
   own keys and overlay are off, so F12 stays the stats' key: the stats box says when a capture can be taken, and what
   the last one wrote (stats_capture). The in-application API is RenderDoc's renderdoc_app.h (MIT, Baldur Karlsson),
   fetched by cmake/dependencies.cmake. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
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
    if (ask) LOG("renderdoc: Ctrl+F12 captures nothing: RenderDoc has no build for NetBSD\n");
    return 0;
}
void vk_renderdoc_present(u64 present) { (void)present; }
#else
#include "renderdoc_app.h"

#define RD_FRAMES 64 /* frames to capture at most */
/* the library's name, the one RenderDoc's own program puts in a game it starts */
#if defined(_WIN32)
#define RD_LIBRARY "renderdoc.dll"
#elif defined(__ANDROID__)
#define RD_LIBRARY "libVkLayer_GLES_RenderDoc.so"
#else
#define RD_LIBRARY "librenderdoc.so"
#endif

typedef struct {
    u64 at;   /* the present, or the time in ms; 0 once taken */
    int secs; /* at is a time */
} RdFrame;
static struct {
    RENDERDOC_API_1_1_2* api; /* NULL: RenderDoc isn't in the process */
    RdFrame frames[RD_FRAMES];
    int nframes;
    int open;  /* a capture runs, to end at the next present */
    int asked; /* Ctrl+F12's: the main thread sets it, the renderer's thread takes it at its next present */
} g_renderdoc;

void vk_renderdoc_load(void) {
    const char *prefix = g_settings.render_doc_captures, *frames = g_settings.render_doc_frames, *p;
    pRENDERDOC_GetAPI get_api;
    /* RenderDoc's program started the game: its library is in the process, and renderdoc= needn't load it */
    void *lib = plat_dl_loaded(RD_LIBRARY), *api = NULL; /* GetAPI stores a void*, which the API's pointer isn't */
    int started = lib != NULL;
    if (!lib && !*prefix) return;
    if (!lib) lib = plat_dl_open(RD_LIBRARY);
    if (!lib) {
        WARN("renderdoc: renderdoc=%s is set but RenderDoc's library, %s, didn't load: %s\n", prefix, RD_LIBRARY,
             plat_dl_error());
        return;
    }
    get_api = (pRENDERDOC_GetAPI)plat_dl_sym(lib, "RENDERDOC_GetAPI");
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2, &api) || !api) {
        WARN("renderdoc: %s has no in-application API 1.1.2: no frame is captured\n", RD_LIBRARY);
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
    LOG("renderdoc: %s, Ctrl+F12 captures the next frame, %d frame(s) listed, to %s\n",
        started ? "in the process (RenderDoc's program started the game)" : "loaded", g_renderdoc.nframes,
        *prefix ? prefix : rd->GetCaptureFilePathTemplate());
    __atomic_store_n(&g_renderdoc.api, rd, __ATOMIC_RELEASE); /* the main thread's Ctrl+F12 reads it */
    stats_capture(&(SnxStatsCapture){.ready = 1});
}

int vk_renderdoc_capture(int ask) {
    if (!__atomic_load_n(&g_renderdoc.api, __ATOMIC_ACQUIRE)) {
        if (ask)
            LOG("renderdoc: Ctrl+F12 captures nothing: RenderDoc isn't in the process (renderdoc= is empty, and "
                "RenderDoc's program didn't start the game)\n");
        return 0;
    }
    if (ask) {
        __atomic_store_n(&g_renderdoc.asked, 1, __ATOMIC_RELEASE);
        LOG("renderdoc: Ctrl+F12: the next frame is captured\n");
    }
    return 1;
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
    want = rd_wanted(present); /* asked once: it takes the frame it answers for */
    if (__atomic_exchange_n(&g_renderdoc.asked, 0, __ATOMIC_ACQ_REL)) want = 1;
    if (g_renderdoc.open || want) vk_rec_sync(); /* the frame's commands are recorded within the capture */
    if (g_renderdoc.open) {
        SnxStatsCapture shown = {.ready = 1};
        g_renderdoc.open = 0;
        if (rd->EndFrameCapture(dev, NULL)) {
            u32 len = 0, n = rd->GetNumCaptures();
            /* GetCapture writes the whole path, unbounded: its length first */
            int told = n && rd->GetCapture(n - 1, NULL, &len, NULL);
            shown.taken = n;
            if (told && len < sizeof shown.path && rd->GetCapture(n - 1, shown.path, NULL, NULL))
                LOG("renderdoc: captured present %llu to %s\n", (unsigned long long)present, shown.path);
            else if (told && len >= sizeof shown.path)
                LOG("renderdoc: captured present %llu, capture %u, to a path of %u bytes, past the %zu kept\n",
                    (unsigned long long)present, n, len, sizeof shown.path);
            else {
                shown.path[0] = 0;
                LOG("renderdoc: captured present %llu, capture %u, to a path RenderDoc doesn't give\n",
                    (unsigned long long)present, n);
            }
        } else {
            shown.taken = rd->GetNumCaptures();
            shown.failed = 1;
            WARN("renderdoc: the capture of present %llu failed\n", (unsigned long long)present);
        }
        stats_capture(&shown);
    }
    if (want) {
        rd->StartFrameCapture(dev, NULL);
        g_renderdoc.open = 1;
    }
}
#endif
