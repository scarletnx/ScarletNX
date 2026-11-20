/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the Ounce's display session (hle/services/gsv.c, nn::vi2::IVi2Session) tells a game of the display's HDR, with
   the renderer stood in for: GetHdrStatus's format (1 for SDR and 2 for HDR, as the SDK's QueryDisplayHdrStatus reads
   it) and peak, GetHdrPaperWhite, and the HDR status event when the display's changes; the largest picture it
   takes, GetBufferMaxResolution; and its refresh rates, with the window's display stood in for: the ones it takes
   (GetDisplayModeCapability), the one it runs at (GetDisplayRefreshRate and its bits), and the game turning 120 Hz
   on and off (301) with the refresh rate event; its state as a save state's load puts it back (g_hle's); and its
   armed syncpoint waits, read and written with its lock held. Everything else gsv.c reaches is stood in for by
   functions no check here calls */
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
Hle g_hle;
HleHost g_host;
Settings g_settings;
_Atomic int g_frames;
u64 g_sync_shim_cpu;
const Renderer* g_renderer;
_Atomic int g_vi_refresh_hz = VI_REFRESH_HZ; /* present.c's, which the session sets */
static u64 g_refresh_ns;             /* the window's display's refresh period, 0 not known (headless) */
u64 backend_display_refresh_ns(void) { return g_refresh_ns; }
/* the locks, of which the armed waits' code takes the supervisor's alone: how deep one is held, and how many times
   the stand-ins that code reaches (a syncpoint read, an event signaled or cleared) ran with none while watched */
static struct {
    int held, watched, unheld;
    u32 syncpt[8]; /* the syncpoints' values, as syncpt_read gives them */
} g_wait;
void pm_lock(PMutex* m) {
    (void)m;
    g_wait.held++;
}
void pm_unlock(PMutex* m) {
    (void)m;
    g_wait.held--;
}
static void reached(void) { g_wait.unheld += g_wait.watched && !g_wait.held; }
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
void kevent_signal(KEvent* e) {
    reached();
    e->signaled = 1;
}
void kevent_clear(KEvent* e) {
    reached();
    e->signaled = 0;
}
u32 ev_handle(u32 ev) { return 0x100 + ev; }
static int signaled(u32 handle) { return g_events[handle - 0x100 - 1].signaled; }
static void clear(u32 handle) { g_events[handle - 0x100 - 1].signaled = 0; }
void kobj_unref(KObj* o) { (void)o; }
/* the transfer memories BindSyncpointMemory is handed: handles 0x201 (read-write) and 0x202 (read-only) */
static KTmem g_tmems[2];
KObj* h_get_ref(u32 h, int t) {
    (void)t;
    return h == 0x201 ? &g_tmems[0].o : h == 0x202 ? &g_tmems[1].o : NULL;
}
int new_event(u32* event, const char* name, int is_signaled) {
    u32 id = owned_event(name);
    g_events[id - 1].signaled = is_signaled != 0;
    *event = ev_handle(id);
    return 0;
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
static u32 g_copied; /* the last handle a reply copied */
void out_copy_h(IpcCtx* c, u32 h) { (void)c, g_copied = h; }
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
    reached();
    return id < 8 ? g_wait.syncpt[id] : 0;
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

/* The camera session (gsv.c's), as a console with no camera plugged in: the SDK's nn::camera::Initialize sends 3 with
   the device name "Default" and aborts on every result but success and 0xdc4, which it gives the game as a platform
   without the camera function; Finalize (4) aborts on any failure. So 3 succeeds with a u64 device handle and 4
   succeeds, 21 gives an event that nothing signals, and every other command, QueryDeviceConnection (29) among them,
   answers 0x7c4 (module 452, description 3), which the SDK lets pass as no camera device */
static void camera_session(void) {
    u8 name[16] = "Default", id[4] = {0};
    IpcCtx c;
    memset(&c, 0, sizeof c);
    c.cmd = 3;
    c.raw = name;
    c.raw_size = sizeof name;
    CHECK(nn_gsv_CameraSession_dispatch(NULL, &c) == 0 && c.out_size == 8 && snx_ld_le64(c.out) == 0);
    memset(&c, 0, sizeof c);
    c.cmd = 4;
    CHECK(nn_gsv_CameraSession_dispatch(NULL, &c) == 0);
    memset(&c, 0, sizeof c);
    c.cmd = 21;
    c.raw = id;
    c.raw_size = sizeof id;
    g_copied = 0;
    CHECK(nn_gsv_CameraSession_dispatch(NULL, &c) == 0 && g_copied && !signaled(g_copied));
    CHECK(!strcmp(g_events[g_copied - 0x100 - 1].o.name, "gsv camera"));
    memset(&c, 0, sizeof c);
    c.cmd = 29;
    c.raw = id;
    c.raw_size = sizeof id;
    CHECK(nn_gsv_CameraSession_dispatch(NULL, &c) == 0x7c4);
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

/* the refresh rates: a display's packet to 301 (the display, a value and a mask of rate codes) and the rate it runs
   at, which GetDisplayRefreshRate and its bits answer alike for 60 and 120 Hz */
static int set_rates(u64 name, u64 value, u64 mask) {
    u8 r[24];
    for (u32 i = 0; i < 8; i++) {
        r[i] = (u8)(name >> (8 * i));
        r[8 + i] = (u8)(value >> (8 * i));
        r[16 + i] = (u8)(mask >> (8 * i));
    }
    return nn_vi2_IVi2Session_SetDisplayRefreshRateCapability(NULL, r);
}
static u64 rate_now(u64 name) {
    u8 r[12];
    u64 rate = 99, bits = 99;
    request(r, name, 0);
    CHECK(!nn_vi2_IVi2Session_GetDisplayRefreshRate(NULL, r, &rate));
    CHECK(!nn_vi2_IVi2Session_GetDisplayRefreshRateBits(NULL, r, &bits) && bits == rate);
    return rate;
}
static u64 rates_offered(void) {
    u8 r[12];
    u64 rates = 99, hdr = 99, rates2 = 99;
    request(r, DEFAULT_DISPLAY, 0);
    CHECK(!nn_vi2_IVi2Session_GetDisplayModeCapability(NULL, r, &rates, &hdr, &rates2) && hdr == 1 && rates2 == 0);
    return rates;
}
/* a command through the session's dispatch, its reply's first 64 bits after the result as the SDK's proxy reads them */
static u64 wire(u32 cmd, u8* raw, u32 size, int* rc) {
    IpcCtx c;
    memset(&c, 0, sizeof c);
    c.cmd = cmd;
    c.tipc = 1;
    c.raw = raw;
    c.raw_size = size;
    *rc = nn_vi2_IVi2Session_dispatch(NULL, &c);
    return *rc || c.out_size < 8 ? 0 : snx_ld_le64(c.out);
}
#define RATE_60  1u
#define RATE_120 2u
#define VRR      (1ull << 63)
static void refresh_rate(void) {
    u8 r[24];
    u32 event = 0;
    int rc;
    /* a 60 Hz display: 60 Hz alone, which turning 120 Hz on leaves as it is */
    g_settings.tv_refresh_rate = 1;
    g_refresh_ns = 1000000000u / 60;
    CHECK(rates_offered() == RATE_60 && rate_now(DEFAULT_DISPLAY) == RATE_60);
    CHECK(!set_rates(DEFAULT_DISPLAY, RATE_120, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_60);
    CHECK(vi_refresh_hz() == 60);
    CHECK(!set_rates(DEFAULT_DISPLAY, 0, RATE_120));
    /* headless and a 95 Hz display: no 120 Hz either */
    g_refresh_ns = 0;
    CHECK(rates_offered() == RATE_60);
    g_refresh_ns = 1000000000u / 95;
    CHECK(rates_offered() == RATE_60);
    /* a 119.88 Hz display takes 120 Hz, and the display runs at it once the game turns it on: the refresh rate event,
       signaled as its group registers so the SDK asks once, is signaled again, so the SDK asks again */
    g_refresh_ns = 1001000000u / 120;
    CHECK(rates_offered() == (RATE_60 | RATE_120));
    request(r, 6, 0);
    CHECK(!nn_vi2_IVi2Session_CreateRequestGroupEvent(NULL, r, &event) && event);
    request(r, DEFAULT_DISPLAY, 6);
    CHECK(!nn_vi2_IVi2Session_RegisterRefreshRateMonitor(NULL, r) && signaled(event));
    clear(event);
    CHECK(!set_rates(DEFAULT_DISPLAY, RATE_120, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_120);
    CHECK(vi_refresh_hz() == 120 && signaled(event));
    clear(event);
    /* the same again, and VRR turned on, change nothing: the runtime offers no VRR */
    CHECK(!set_rates(DEFAULT_DISPLAY, RATE_120, RATE_120) && !set_rates(DEFAULT_DISPLAY, VRR, VRR));
    CHECK(rate_now(DEFAULT_DISPLAY) == RATE_120 && !signaled(event));
    /* a guest's display of GameShare runs at 60 Hz, and turning its 120 Hz off leaves the console's on */
    CHECK(rate_now(0x0031726579616c70ull /* "player1" */) == RATE_60);
    CHECK(!set_rates(0x0031726579616c70ull, 0, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_120);
    /* 120 Hz off, as a part made for a 4K TV turns it: 60 Hz again, the event signaled */
    CHECK(!set_rates(DEFAULT_DISPLAY, 0, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_60);
    CHECK(vi_refresh_hz() == 60 && signaled(event));
    clear(event);
    /* on the wire: 354 and 103 put the rate after the result, 301 takes the display, the value and the mask */
    request(r, DEFAULT_DISPLAY, 0);
    CHECK(wire(354, r, 8, &rc) == RATE_60 && !rc);
    snx_st_le64(r + 8, RATE_120);
    snx_st_le64(r + 16, RATE_120);
    wire(301, r, 0x18, &rc);
    CHECK(!rc && wire(103, r, 8, &rc) == RATE_120 && !rc && wire(354, r, 8, &rc) == RATE_120 && !rc);
    CHECK(set_rates(0x1234, RATE_120, RATE_120) != 0); /* no such display */
    request(r, 0x1234, 0);
    wire(103, r, 8, &rc);
    CHECK(rc != 0);
    clear(event);
    /* unregistered: a change is no longer signaled */
    request(r, DEFAULT_DISPLAY, 6);
    CHECK(!nn_vi2_IVi2Session_UnregisterRefreshRateMonitor(NULL, r));
    CHECK(!set_rates(DEFAULT_DISPLAY, 0, RATE_120) && vi_refresh_hz() == 60 && !signaled(event));
    /* tv_refresh_rate 0: never 120 Hz; 2: 120 Hz on a 60 Hz display too */
    g_settings.tv_refresh_rate = 0;
    g_refresh_ns = 1000000000u / 120;
    CHECK(rates_offered() == RATE_60);
    CHECK(!set_rates(DEFAULT_DISPLAY, RATE_120, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_60);
    g_settings.tv_refresh_rate = 2;
    g_refresh_ns = 1000000000u / 60;
    CHECK(rates_offered() == (RATE_60 | RATE_120));
    CHECK(!set_rates(DEFAULT_DISPLAY, RATE_120, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_120);
    CHECK(!set_rates(DEFAULT_DISPLAY, 0, RATE_120) && rate_now(DEFAULT_DISPLAY) == RATE_60);
}

/* The supervisor's state is g_hle's, which a save state writes and a load puts back: a native memory made after the
   state was taken is gone once it is loaded, and the next one takes its handle again. Kept in the file's own
   variables, the state stayed the running process's whatever was loaded */
static void state_in_hle(void) {
    u8 request[12] = {0};
    u32 first = 0, second = 0, again = 0;
    Hle* state = malloc(sizeof g_hle);
    CHECK(state != NULL);
    snx_st_le64(request + 4, 0x4000);
    CHECK(!nn_gsv_UserSession_CreateNativeMemory(NULL, request, &first) && first);
    memcpy(state, &g_hle, sizeof g_hle);
    CHECK(!nn_gsv_UserSession_CreateNativeMemory(NULL, request, &second) && second == first + 1);
    memcpy(&g_hle, state, sizeof g_hle);
    CHECK(!nn_gsv_UserSession_CreateNativeMemory(NULL, request, &again) && again == second);
    free(state);
}

/* A syncpoint wait armed short of its threshold is signaled when the syncpoint gets there, one armed at it at once,
   and a cancelled one too, as the SDK waits on the event after it cancels. A guest's thread arms and cancels while
   the GPU's moves the syncpoints: every read of a syncpoint and every signal the waits' code makes runs with the
   supervisor's lock held */
static void armed_waits(void) {
    u8 request[16];
    u32 event = 0;
    snx_st_le32(request, 3);       /* the slot */
    snx_st_le32(request + 4, ~5u); /* syncpoint 5: the SDK negates the id in the packet */
    snx_st_le64(request + 8, 10);  /* the threshold */
    g_wait.watched = 1;
    CHECK(!nn_gsv_SyncptSession_GetEvent(NULL, 3, &event) && event);
    g_wait.syncpt[5] = 7;
    CHECK(!nn_gsv_SyncptSession_ArmWait(NULL, request) && !signaled(event));
    gsv_syncpt_changed();
    CHECK(!signaled(event));
    g_wait.syncpt[5] = 10;
    gsv_syncpt_changed();
    CHECK(signaled(event));
    CHECK(!nn_gsv_SyncptSession_ArmWait(NULL, request) && signaled(event)); /* there already: cleared, then signaled */
    clear(event);
    CHECK(!nn_gsv_SyncptSession_CancelWait(NULL, 3) && signaled(event));
    g_wait.watched = 0;
    CHECK(g_wait.unheld == 0 && g_wait.held == 0);
}

/* BindSyncpointMemory fills 0x2000 bytes of the read-only memory and reads the read-write one's first 0x20: a memory
   shorter than its part is refused, and nothing past its end is written */
static void syncpoint_memory(void) {
    static u8 rw[0x1000], ro[0x3000];
    u8 request[8] = {0};
    u32 asked = 0, done = 0;
    memset(ro, 0xEE, sizeof ro);
    g_tmems[0] = (KTmem){.addr = (u64)(uintptr_t)rw, .size = sizeof rw};
    g_tmems[1] = (KTmem){.addr = (u64)(uintptr_t)ro, .size = 0x1000};
    CHECK(nn_gsv_UserSession_BindSyncpointMemory(NULL, 0x201, 0x202, request, &asked, &done) != 0);
    CHECK(ro[0] == 0xEE && ro[0x1000] == 0xEE && ro[0x1FFF] == 0xEE);
    g_tmems[0].size = 0x10, g_tmems[1].size = 0x2000;
    CHECK(nn_gsv_UserSession_BindSyncpointMemory(NULL, 0x201, 0x202, request, &asked, &done) != 0 && ro[0] == 0xEE);
    CHECK(nn_gsv_UserSession_BindSyncpointMemory(NULL, 0x201, 0x203, request, &asked, &done) != 0);
    g_tmems[0].size = sizeof rw;
    CHECK(nn_gsv_UserSession_BindSyncpointMemory(NULL, 0x201, 0x202, request, &asked, &done) == 0 && asked && done);
    CHECK(ro[0] == 1 && ro[0x1FFF] == 0 && ro[0x2000] == 0xEE);
}

/* a client's token is 64 bits, and a display's endpoint keeps the client by the low 32: a client that goes takes its
   endpoint with it whatever its token's high bits, so the next client opens the display's */
static void endpoint_of_a_gone_client(void) {
    u8 client[16], open[24];
    const u64 token = 0x100000002ull;
    snx_st_le64(client, token), snx_st_le32(client + 8, 3), snx_st_le32(client + 12, 4);
    snx_st_le64(open, DEFAULT_DISPLAY), snx_st_le64(open + 8, DEFAULT_DISPLAY), snx_st_le64(open + 16, token);
    CHECK(!nn_vi2_IVi2Session_RegisterClientState(NULL, client) && !nn_vi2_IVi2Session_OpenEndpoint(NULL, open));
    CHECK(nn_vi2_IVi2Session_OpenEndpoint(NULL, open) != 0); /* held */
    CHECK(!nn_vi2_IVi2Session_UnregisterClientState(NULL, client));
    CHECK(!nn_vi2_IVi2Session_RegisterClientState(NULL, client) && !nn_vi2_IVi2Session_OpenEndpoint(NULL, open));
    CHECK(!nn_vi2_IVi2Session_CloseEndpoint(NULL, open) && !nn_vi2_IVi2Session_UnregisterClientState(NULL, client));
}
int main(void) {
    u8 r[12], other[12];
    endpoint_of_a_gone_client();
    u32 hdr_event = 0, vsync_event = 0;
    max_resolution();
    refresh_rate();
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
    camera_session();
    state_in_hle();
    armed_waits();
    syncpoint_memory();
    printf("gsv hdr: %u checks\n", test_checks);
    return 0;
}
