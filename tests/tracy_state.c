/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Tracy client's lifetime (src/nx/runtime/platform/tracy.c) over a stand-in client: the emissions' gate shut
   before the client starts, open while it runs and shut for good once shutdown has run; the client started and shut
   down once each, the zone shutdown finds open ended once, and a capture asked for after the shutdown starts nothing.
   The GPU calibration anchor is kept across a connection: the context is announced once, so a new connection replays
   that announcement and the caller's delta starts from it again. */
#include "nx/runtime/platform/tracy.c"

/* the checks failed, and what tracy.c asked of the stand-in client */
static struct {
    int failures;
    int startups, shutdowns, connected, zones, zone_ends, captures, logs, frames;
    int gpu_begins, gpu_ends, gpu_times, gpu_syncs, gpu_calibrates, configs, srclocs;
    uint64_t conn;
} g_test;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "tracy_state:%d: %s\n", __LINE__, #x);                                                     \
            g_test.failures++;                                                                                         \
        }                                                                                                              \
    } while (0)

/* the stand-in client */
void ___tracy_startup_profiler(void) { g_test.startups++; }
void ___tracy_shutdown_profiler(void) { g_test.shutdowns++; }
int32_t ___tracy_connected(void) { return g_test.connected; }
TracyCZoneCtx ___tracy_emit_zone_begin(const struct ___tracy_source_location_data* srcloc, int32_t active) {
    TracyCZoneCtx ctx = {0};
    (void)srcloc;
    ctx.id = (uint32_t)++g_test.zones;
    ctx.active = active;
    return ctx;
}
void ___tracy_emit_zone_end(TracyCZoneCtx ctx) {
    (void)ctx;
    g_test.zone_ends++;
}
void ___tracy_set_thread_name(const char* name) { (void)name; }
void ___tracy_emit_message_appinfo(const char* txt, size_t size) { (void)txt, (void)size; }
void ___tracy_emit_plot(const char* name, double val) { (void)name, (void)val; }
void ___tracy_emit_plot_config(const char* name, int32_t type, int32_t step, int32_t fill, uint32_t color) {
    (void)name, (void)type, (void)step, (void)fill, (void)color;
    g_test.configs++;
}
int ___tracy_begin_sampling_profiling(void) { return 0; }
void ___tracy_end_sampling_profiling(void) {}
void ___tracy_emit_frame_mark_start(const char* name) { (void)name; }
void ___tracy_emit_frame_mark_end(const char* name) { (void)name; }
void ___tracy_emit_logString(int8_t severity, int32_t color, int32_t callstack_depth, size_t size, const char* txt) {
    (void)severity, (void)color, (void)callstack_depth, (void)size, (void)txt;
}
uint64_t ___tracy_alloc_srcloc_name(uint32_t line, const char* source, size_t source_size, const char* function,
                                    size_t function_size, const char* name, size_t name_size, uint32_t color) {
    (void)line, (void)source, (void)source_size, (void)function, (void)function_size, (void)name, (void)name_size;
    (void)color;
    /* the client's own block for the name: a fresh one each call, as its malloc does */
    return (uint64_t)(++g_test.srclocs) + 1;
}
void ___tracy_emit_gpu_new_context_serial(const struct ___tracy_gpu_new_context_data d) {
    (void)d;
    g_test.gpu_begins++; /* the context itself */
}
void ___tracy_emit_gpu_context_name_serial(const struct ___tracy_gpu_context_name_data d) {
    (void)d;
    g_test.gpu_begins++; /* the name announced with it */
}
void ___tracy_emit_gpu_zone_begin_alloc_serial(const struct ___tracy_gpu_zone_begin_data d) {
    (void)d;
    g_test.gpu_begins++;
}
void ___tracy_emit_gpu_zone_end_serial(const struct ___tracy_gpu_zone_end_data d) {
    (void)d;
    g_test.gpu_ends++;
}
void ___tracy_emit_gpu_time_serial(const struct ___tracy_gpu_time_data d) {
    (void)d;
    g_test.gpu_times++;
}
void ___tracy_emit_gpu_time_sync_serial(const struct ___tracy_gpu_time_sync_data d) {
    (void)d;
    g_test.gpu_syncs++;
}
void ___tracy_emit_gpu_calibration_serial(const struct ___tracy_gpu_calibration_data d) {
    (void)d;
    g_test.gpu_calibrates++;
}
void ___tracy_emit_frame_image(const void* image, uint16_t w, uint16_t h, uint8_t offset, int32_t flip) {
    (void)image, (void)w, (void)h, (void)offset, (void)flip;
    g_test.frames++;
}
struct __tracy_lockable_context_data* ___tracy_announce_lockable_ctx(const struct ___tracy_source_location_data* s) {
    (void)s;
    return NULL;
}
void ___tracy_custom_name_lockable_ctx(struct __tracy_lockable_context_data* l, const char* name, size_t size) {
    (void)l, (void)name, (void)size;
}
void ___tracy_terminate_lockable_ctx(struct __tracy_lockable_context_data* l) { (void)l; }
int32_t ___tracy_before_lock_lockable_ctx(struct __tracy_lockable_context_data* l) {
    (void)l;
    return 0;
}
void ___tracy_after_lock_lockable_ctx(struct __tracy_lockable_context_data* l) { (void)l; }
void ___tracy_before_unlock_lockable_ctx(struct __tracy_lockable_context_data* l) { (void)l; }
void ___tracy_after_try_lock_lockable_ctx(struct __tracy_lockable_context_data* l, int32_t acquired) {
    (void)l, (void)acquired;
}
uint64_t snx_tracy_connection_id(void) { return g_test.conn ? g_test.conn : 1; }

/* the runtime around it */
char g_datadir[1024], g_exename[256], g_exedir[1024];
int g_opt_headless, g_opt_tracy_capture, g_opt_tracy_client;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl, (void)fmt;
    g_test.logs++;
}
int plat_setenv(const char* name, const char* value) {
    (void)name, (void)value;
    return 0;
}
SDL_Process* SDL_CreateProcess(const char* const* args, bool pipe_stdio) {
    (void)args, (void)pipe_stdio;
    g_test.captures++;
    return NULL;
}
bool SDL_WaitProcess(SDL_Process* process, bool block, int* exitcode) {
    (void)process, (void)block, (void)exitcode;
    return true;
}
void SDL_DestroyProcess(SDL_Process* process) { (void)process; }
const char* SDL_GetError(void) { return "no process in this test"; }

int main(void) {
    static const struct ___tracy_source_location_data where = {NULL, "zone", __FILE__, __LINE__, 0};
    TracyCZoneCtx ctx;
    int logs;
    unsigned dropped0;
    /* before the start: the gate shut, and a shutdown has nothing to stop, so the start still comes */
    CHECK(!snx_tracy_gate_open() && !snx_tracy_enter());
    CHECK(!snx_tracy_zone_begin(&where).active);
    snx_tracy_shutdown();
    CHECK(g_test.shutdowns == 0);
    g_opt_tracy_client = 1;
    CHECK(snx_tracy_init("tracy_state") == 0 && g_test.startups == 1);
    /* running: the gate open, a zone emitted once a server is attached */
    CHECK(snx_tracy_gate_open() && snx_tracy_enter());
    snx_tracy_leave();
    CHECK(!snx_tracy_zone_begin(&where).active); /* nobody attached */
    g_test.connected = 1;
    ctx = snx_tracy_zone_begin(&where);
    CHECK(ctx.active && g_test.zones == 1);
    snx_tracy_zone_end(ctx);
    CHECK(g_test.zone_ends == 1 && g_tracy_pending == 0);
    snx_tracy_capture_now(); /* the GUI is attached: no capture, and the client is not started again */
    CHECK(g_test.startups == 1 && g_test.captures == 0);
    /* a GPU context is announced once for the client's lifetime: a new connection replays that announcement instead
       of a second one, so the caller keeps the anchor it took at the first (SnxTracyGpuCal) */
    g_test.conn = 1;
    CHECK(snx_tracy_gpu_context(0, 100, 1.0f, 1, "graphics") == 1);
    g_test.conn = 2;
    CHECK(snx_tracy_gpu_context(0, 100, 1.0f, 1, "graphics") == 0);
    /* its zones across the connections: a pair goes out with the connection of its begin, and an end or a time from
       another one is dropped, where the server would fault on a query it never saw */
    {
        uint64_t ep;
        u16 q = snx_tracy_gpu_zone_begin("gpu pass", 0, 0, &ep), e;
        CHECK(q && g_test.gpu_begins == 3); /* the announcement's pair (the second is sent nothing) and the zone */
        /* the name's block is the client's own, one per zone: it frees it once it has sent it, so a runtime that
           kept one per name would hand it a freed block, which aborted the game */
        CHECK(g_test.srclocs == 1);
        snx_tracy_gpu_zone_begin("gpu pass", 0, 0, &ep); /* the same name again: a block of its own */
        CHECK(g_test.srclocs == 2);
        e = snx_tracy_gpu_zone_end(q, 0, ep);
        CHECK(e && g_test.gpu_ends == 1);
        snx_tracy_gpu_time(1000, q, 0, ep); /* the pair the read-back sends: its begin and its end */
        snx_tracy_gpu_time(2000, e, 0, ep);
        CHECK(g_test.gpu_times == 2);
        snx_tracy_gpu_time(3000, q, 0, ep + 1); /* another connection's: dropped, its query is not the server's */
        snx_tracy_gpu_zone_end(q, 0, ep + 1);
        snx_tracy_gpu_sync(4000, 0); /* the uncalibrated context's anchor: the clock it is given */
        CHECK(g_test.gpu_times == 2 && g_test.gpu_ends == 1 && g_test.gpu_syncs == 1);
        /* each end it is given goes out: a caller that sends one twice sends two, which the server pairs by query */
        snx_tracy_gpu_zone_end(q, 0, ep);
        CHECK(g_test.gpu_ends == 2);
    }
    /* the anchor is the announcement's pair, and a reconnect measures from it again rather than from the last event */
    {
        SnxTracyGpuCal cal = {0};
        snx_tracy_gpu_cal_announce(&cal, 1, 1000);
        CHECK(cal.calibrated && cal.cpu_anchor == 1000 && cal.cpu_prev == 1000);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1200) == 200 && cal.cpu_prev == 1200);
        g_test.conn = 3; /* a new connection: its server is back at the replayed announcement's pair */
        snx_tracy_gpu_cal_reconnect(&cal);
        CHECK(cal.cpu_prev == 1000);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1300) == 300 && cal.cpu_prev == 1300);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1300) == 0 && cal.cpu_prev == 1300); /* a clock that stands still */
        snx_tracy_gpu_cal_reconnect(&cal); /* the same connection leaves the anchor where the last event put it */
        CHECK(cal.cpu_prev == 1300);
        /* an announcement without a clock pair latches uncalibrated: a delta would trip the server's assert */
        snx_tracy_gpu_cal_announce(&cal, 0, 0);
        snx_tracy_gpu_cal_reconnect(&cal);
        CHECK(!cal.calibrated && cal.cpu_anchor == 0 && cal.cpu_prev == 0);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 5000) == 0 && cal.cpu_prev == 0);
        /* a calibrated context whose clock read 0 at the announcement starts at the next pair */
        snx_tracy_gpu_cal_announce(&cal, 1, 0);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 700) == 0 && cal.cpu_prev == 700);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 900) == 200);
    }
    dropped0 = g_tracy_dropped; /* what the run dropped before the test's own zones: the app's code's */
    /* a zone deeper than the limit is dropped rather than leaked: its begin sends nothing, and the count says how
       many went that way */
    {
        TracyCZoneCtx deep[70];
        int i;
        g_test.zones = g_test.zone_ends = 0;
        for (i = 0; i < 70; i++) deep[i] = snx_tracy_zone_begin(&where);
        CHECK(g_test.zones == (int)SNX_TRACY_ZONE_MAX);
        CHECK(g_tracy_dropped == (unsigned)(70 - (int)SNX_TRACY_ZONE_MAX) + dropped0);
        CHECK(g_tracy_pending == (int)SNX_TRACY_ZONE_MAX); /* each zone it began is in flight */
        for (i = 0; i < 70; i++)
            if (deep[i].active) snx_tracy_zone_end(deep[i]);
        CHECK(g_tracy_pending == 0 && g_test.zone_ends == (int)SNX_TRACY_ZONE_MAX);
        /* out of order: the stack compacts, and each end reaches the client once */
        deep[0] = snx_tracy_zone_begin(&where);
        deep[1] = snx_tracy_zone_begin(&where);
        snx_tracy_zone_end(deep[0]); /* the outer one first */
        snx_tracy_zone_end(deep[1]);
        CHECK(g_test.zone_ends == (int)SNX_TRACY_ZONE_MAX + 2 && g_tracy_pending == 0);
    }
    /* shut down with a zone open: the zone ended once, the gate shut, the client shut down once */
    ctx = snx_tracy_zone_begin(&where);
    CHECK(ctx.active && g_tracy_pending == 1);
    snx_tracy_shutdown();
    CHECK(g_test.shutdowns == 1 && g_test.zone_ends == 67 && g_tracy_pending == 0); /* the deep test's, and this one's */
    snx_tracy_zone_end(ctx);
    CHECK(g_test.zone_ends == 67);
    CHECK(!snx_tracy_gate_open() && !snx_tracy_enter() && !snx_tracy_zone_begin(&where).active);
    /* stopped for good: a second shutdown and a capture asked for start and stop nothing, and say nothing either
       (a running client's capture would say the GUI is attached) */
    logs = g_test.logs;
    snx_tracy_shutdown();
    snx_tracy_capture_now();
    CHECK(g_test.shutdowns == 1 && g_test.startups == 1 && g_test.captures == 0 && g_test.logs == logs);
    CHECK(!snx_tracy_gate_open());
    if (g_test.failures) return 1;
    printf("tracy_state: the gate shut before the start and after the stop, the client started and stopped once, "
           "the GPU calibration anchor survived a reconnect\n");
    return 0;
}
