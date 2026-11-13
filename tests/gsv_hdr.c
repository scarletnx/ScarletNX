/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the Ounce's display session (hle/services/gsv.c, nn::vi2::IVi2Session) tells a game of the display's HDR, with
   the renderer stood in for: GetHdrStatus's format (1 for SDR and 2 for HDR, as the SDK's QueryDisplayHdrStatus reads
   it) and peak, GetHdrPaperWhite, and the HDR status event when the display's changes; and the largest picture it
   takes, GetBufferMaxResolution. Everything else gsv.c reaches is stood in for by functions no check here calls */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/hle/services/gsv.c"

static unsigned test_checks;
static void test_fail(const char* expression, int line) {
    fprintf(stderr, "gsv_hdr.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)
#define UNUSED(name) test_fail("a check reached " name ", which this test does not stand in for", __LINE__)

/* ---- what gsv.c links against ---- */
u8* g_mem;
MemLayout g_layout;
HleHost g_host;
Settings g_settings;
int g_frames;
u64 g_sync_shim_cpu;
const Renderer* g_renderer;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
/* events: an id is its place in g_events plus 1, as the kernel's object table hands ids out */
static KEvent g_events[16];
static u32 g_event_count;
u32 owned_event(const char* name) {
    CHECK(g_event_count < 16);
    g_events[g_event_count].o.type = KO_EVENT;
    kobj_set_name(&g_events[g_event_count].o, name);
    return ++g_event_count;
}
KObj* kobj_at(u32 id) { return id && id <= g_event_count ? &g_events[id - 1].o : NULL; }
void kevent_signal(KEvent* e) { e->signaled = 1; }
void kevent_clear(KEvent* e) { e->signaled = 0; }
u32 ev_handle(u32 ev) { return 0x100 + ev; }
static int signaled(u32 handle) { return g_events[handle - 0x100 - 1].signaled; }
static void clear(u32 handle) { g_events[handle - 0x100 - 1].signaled = 0; }
void kobj_unref(KObj* o) { (void)o; }
KObj* h_get_ref(u32 h, int t) {
    (void)h, (void)t;
    UNUSED("h_get_ref");
    return NULL;
}
int new_event(u32* event, const char* name, int is_signaled) {
    (void)event, (void)name, (void)is_signaled;
    UNUSED("new_event");
    return -1;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    (void)name, (void)dispatch;
    UNUSED("svcobj_new");
    return NULL;
}
u32 session_new(SvcObj* obj) {
    (void)obj;
    UNUSED("session_new");
    return 0;
}
void out_copy_h(IpcCtx* c, u32 h) { (void)c, (void)h; }
void out_move_h(IpcCtx* c, u32 h) { (void)c, (void)h; }
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 221);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    UNUSED("stub_ok");
    return -1;
}
int nns_nvdrv_INvDrvServices_dispatch(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void gmmu_map(u64 gva, u64 cpu, u64 size) {
    (void)gva, (void)cpu, (void)size;
    UNUSED("gmmu_map");
}
u64 mem_mapped_run(u64 addr, u64 size) {
    (void)addr, (void)size;
    UNUSED("mem_mapped_run");
    return 0;
}
void mem_query(u64 addr, MemBlock* out) {
    (void)addr, (void)out;
    UNUSED("mem_query");
}
u32 syncpt_alloc(void) {
    UNUSED("syncpt_alloc");
    return 0;
}
u32 syncpt_read(u32 id) {
    (void)id;
    UNUSED("syncpt_read");
    return 0;
}
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg) {
    (void)n, (void)id, (void)val, (void)fn, (void)arg;
    UNUSED("nv_gpu_call_after");
}
int native_video_service(int encoder, int enable) {
    (void)encoder, (void)enable;
    return -1;
}
int native_video_open(int encoder, u32* session) {
    (void)encoder, (void)session;
    return -1;
}
int native_video_requirements(const u32 pref[16], u32 count, u64* size, u64* alignment) {
    (void)pref, (void)count, (void)size, (void)alignment;
    return -1;
}
int native_video_pool_create(const u32 pref[16], u32 count, u32 memory, u32* image) {
    (void)pref, (void)count, (void)memory, (void)image;
    return -1;
}
int native_video_pool_destroy(u32 image) {
    (void)image;
    return -1;
}
int native_video_query(u32 image, u64 d[16]) {
    (void)image, (void)d;
    return -1;
}
int native_video_gpu_query(u32 image, u64 d[16]) {
    (void)image, (void)d;
    return -1;
}
int native_video_gpu_map(u32 image, u32 address_space, u64* first, u64* last, u64 d[16]) {
    (void)image, (void)address_space, (void)first, (void)last, (void)d;
    return -1;
}
int native_video_gpu_unmap(u32 image, u32 address_space, u64 first, u64 last) {
    (void)image, (void)address_space, (void)first, (void)last;
    return -1;
}

/* ---- the renderer's side: whether the display shows a game's HDR frames, and the two numbers of the settings ---- */
static int g_hdr;
static int test_hdr_display(u32* peak, u32* paper_white) {
    if (!g_hdr) return 0;
    *peak = 1600;
    *paper_white = 250;
    return 1;
}
static const Renderer g_with = {.name = "test", .hdr_display = test_hdr_display};
static const Renderer g_without = {.name = "test without HDR"};

#define DEFAULT_DISPLAY 0x00746c7561666564ull /* "default" */
/* a display's name and a request group, as the session's packets hold them: little-endian */
static void request(u8 out[12], u64 name, u32 group) {
    for (u32 i = 0; i < 8; i++) out[i] = (u8)(name >> (8 * i));
    for (u32 i = 0; i < 4; i++) out[8 + i] = (u8)(group >> (8 * i));
}
/* GetHdrStatus's reply as the SDK's proxy reads it from the wire: the format's 64 bits at the start of the raw data and
   the peak's 16 bits at its byte 8. Laid out by alignment instead, the peak first, an HDR display's reply reads as
   format 1600 there, which the SDK takes for an error and the game for an SDR display */
static void status_wire(u64 name, u64 want_format, u32 want_peak) {
    u8 r[12];
    IpcCtx c;
    request(r, name, 0);
    memset(&c, 0, sizeof c);
    c.cmd = 104;
    c.tipc = 1;
    c.raw = r;
    c.raw_size = 8; /* the display's name alone */
    CHECK(nn_vi2_IVi2Session_dispatch(NULL, &c) == 0);
    CHECK(c.out_size >= 10 && snx_ld_le64(c.out) == want_format && snx_ld_le16(c.out + 8) == want_peak);
}
static void status(u64 name, int rc_ok, u64 want_format, u32 want_peak, u32 want_paper_white) {
    u8 r[12];
    u64 format = 99;
    u32 peak = 99, paper_white = 99;
    request(r, name, 0);
    CHECK((nn_vi2_IVi2Session_GetHdrStatus(NULL, r, &format, &peak) == 0) == rc_ok);
    if (rc_ok) CHECK(format == want_format && peak == want_peak);
    if (rc_ok) status_wire(name, want_format, want_peak);
    CHECK((nn_vi2_IVi2Session_GetHdrPaperWhite(NULL, r, &paper_white) == 0) == rc_ok);
    if (rc_ok) CHECK(paper_white == want_paper_white);
}

/* GetBufferMaxResolution: one bit for the largest picture the display takes (1 1280x720, 2 1920x1080, 8 3840x2160),
   a TV's in TV mode and the console's own screen in handheld mode */
static void max_resolution(void) {
    u8 r[12];
    u64 resolution = 0;
    request(r, DEFAULT_DISPLAY, 0);
    g_settings.tv_mode = 1;
    CHECK(!nn_vi2_IVi2Session_GetBufferMaxResolution(NULL, r, &resolution) && resolution == 8);
    g_settings.tv_mode = 0;
    CHECK(!nn_vi2_IVi2Session_GetBufferMaxResolution(NULL, r, &resolution) && resolution == 2);
    request(r, 0x1234, 0); /* no such display */
    CHECK(nn_vi2_IVi2Session_GetBufferMaxResolution(NULL, r, &resolution) != 0);
}

int main(void) {
    u8 r[12], other[12];
    u32 hdr_event = 0, vsync_event = 0;
    max_resolution();
    /* no renderer, one that shows no HDR at all, and one whose display is SDR: format 1, no peak, no paper white */
    status(DEFAULT_DISPLAY, 1, 1, 0, 0);
    g_renderer = &g_without;
    status(DEFAULT_DISPLAY, 1, 1, 0, 0);
    g_renderer = &g_with;
    status(DEFAULT_DISPLAY, 1, 1, 0, 0);
    /* an HDR display: format 2, and the renderer's peak and paper white */
    g_hdr = 1;
    status(DEFAULT_DISPLAY, 1, 2, 1600, 250);
    status(0x0031726579616c70ull /* "player1" */, 1, 2, 1600, 250);
    status(0x1234, 0, 0, 0, 0); /* no such display */
    g_hdr = 0;
    status(DEFAULT_DISPLAY, 1, 1, 0, 0);

    /* The HDR status event: signaled when a group registers for it, so the SDK asks once, and then by the vsync tick
       only when the answer has changed since the tick before */
    request(r, 3, 0);
    CHECK(!nn_vi2_IVi2Session_CreateRequestGroupEvent(NULL, r, &hdr_event) && hdr_event);
    request(other, 5, 0);
    CHECK(!nn_vi2_IVi2Session_CreateRequestGroupEvent(NULL, other, &vsync_event) && vsync_event != hdr_event);
    request(r, DEFAULT_DISPLAY, 3);
    request(other, DEFAULT_DISPLAY, 5);
    CHECK(!nn_vi2_IVi2Session_RegisterHdrStatusMonitor(NULL, r) && signaled(hdr_event));
    CHECK(!nn_vi2_IVi2Session_RegisterDisplayEventMonitor(NULL, other) && !signaled(vsync_event));
    clear(hdr_event);
    gsv_vsync_tick(); /* the first tick has nothing to compare with */
    CHECK(!signaled(hdr_event) && signaled(vsync_event));
    gsv_vsync_tick();
    CHECK(!signaled(hdr_event));
    g_hdr = 1; /* the window moved to an HDR display */
    gsv_vsync_tick();
    CHECK(signaled(hdr_event));
    status(DEFAULT_DISPLAY, 1, 2, 1600, 250);
    clear(hdr_event);
    gsv_vsync_tick();
    CHECK(!signaled(hdr_event));
    g_hdr = 0; /* and back */
    gsv_vsync_tick();
    CHECK(signaled(hdr_event));
    clear(hdr_event);
    /* the vsync group is no HDR status group, and unregistering it leaves the HDR one registered */
    clear(vsync_event);
    CHECK(!nn_vi2_IVi2Session_UnregisterDisplayEventMonitor(NULL, other));
    g_hdr = 1;
    gsv_vsync_tick();
    CHECK(signaled(hdr_event) && !signaled(vsync_event));
    clear(hdr_event);
    /* unregistered: a change is no longer signaled */
    CHECK(!nn_vi2_IVi2Session_UnregisterHdrStatusMonitor(NULL, r));
    g_hdr = 0;
    gsv_vsync_tick();
    CHECK(!signaled(hdr_event));
    printf("gsv hdr: %u checks\n", test_checks);
    return 0;
}
