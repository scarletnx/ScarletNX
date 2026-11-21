/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Tracy client's lifetime (src/nx/runtime/platform/tracy.c) over a stand-in client: the emissions' gate shut
   before the client starts, open while it runs and shut for good once shutdown has run; the client started and shut
   down once each, the zone shutdown finds open ended once, and a capture asked for after the shutdown starts nothing.
   The GPU calibration anchor is kept across a connection: the context is announced once, so a new connection replays
   that announcement and the caller's delta starts from it again. */
#include "nx/runtime/platform/tracy.c"
#include <assert.h>
#include <pthread.h>

/* the checks failed, and what tracy.c asked of the stand-in client */
static struct {
    int failures;
    int startups, shutdowns, connected, zones, zone_ends, captures, logs, frames;
    int fibers, fiber_leaves;
    int gpu_begins, gpu_ends, gpu_times, gpu_syncs, gpu_calibrates, configs, srclocs;
    int gpu_callstacks, transient_begins, transient_callstacks;
    int callstacks, callstack_depth;
    int msgs, samplings;
    int8_t severity;
    size_t msg_size;
    char msg[32];
    int64_t gpu_time, gpu_sync_time;
    float gpu_period;
    uint8_t gpu_ctx;
    uint64_t conn;
} g_test;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "tracy_state:%d: %s\n", __LINE__, #x);                                                     \
            g_test.failures++;                                                                                         \
        }                                                                                                              \
    } while (0)

/* a host thread whose first unnamed frame mark on the current connection must be a seed */
static void* frame_mark_seed(void* arg) {
    (void)arg;
    snx_tracy_frame_mark();
    return NULL;
}
/* a name expression with a side effect: one SNX_PLOT_CFG call must increment this once */
static int g_plot_name_evals;
static const char* plot_name(void) {
    g_plot_name_evals++;
    return "a cfg plot";
}
static void* plot_cfg_thread(void* arg) {
    (void)arg;
    SNX_PLOT_CFG(plot_name(), 1.0, 2, 0, 1, 0xff0000u);
    SNX_PLOT_CFG(plot_name(), 1.0, 2, 0, 1, 0xff0000u);
    return NULL;
}

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
TracyCZoneCtx ___tracy_emit_zone_begin_callstack(const struct ___tracy_source_location_data* srcloc, int32_t depth,
                                                  int32_t active) {
    g_test.callstacks++;
    g_test.callstack_depth = (int)depth;
    return ___tracy_emit_zone_begin(srcloc, active);
}
void ___tracy_emit_zone_end(TracyCZoneCtx ctx) {
    (void)ctx;
    g_test.zone_ends++;
}
TracyCZoneCtx ___tracy_emit_zone_begin_alloc(uint64_t srcloc, int32_t active) {
    TracyCZoneCtx ctx = {0};
    (void)srcloc;
    ctx.id = (uint32_t)++g_test.zones;
    ctx.active = active;
    g_test.transient_begins++;
    return ctx;
}
TracyCZoneCtx ___tracy_emit_zone_begin_alloc_callstack(uint64_t srcloc, int32_t depth, int32_t active) {
    g_test.transient_callstacks++;
    g_test.callstack_depth = (int)depth;
    return ___tracy_emit_zone_begin_alloc(srcloc, active);
}
void ___tracy_set_thread_name(const char* name) { (void)name; }
uint32_t ___tracy_section_enter(uint16_t category, const char* fmt, ...) {
    (void)category, (void)fmt;
    return 1;
}
void ___tracy_section_setup(uint16_t category, const char* fmt, ...) { (void)category, (void)fmt; }
void ___tracy_section_leave(uint32_t id) { (void)id; }
void snx_tracy_conn_set_program_name(const char* name) { (void)name; }
void snx_tracy_conn_register_parameters(void) {}
void snx_tracy_conn_register_source_callback(void) {}
void snx_tracy_conn_thread_name_hint(const char* name, const char* hint) { (void)name, (void)hint; }
void ___tracy_emit_gpu_zone_begin_alloc_callstack_serial(struct ___tracy_gpu_zone_begin_callstack_data data) {
    (void)data;
    g_test.gpu_callstacks++;
}
void ___tracy_mark_lockable_ctx(struct __tracy_lockable_context_data* lockdata,
                                const struct ___tracy_source_location_data* srcloc) {
    (void)lockdata, (void)srcloc;
}
void ___tracy_emit_message_appinfo(const char* txt, size_t size) { (void)txt, (void)size; }
void ___tracy_emit_plot(const char* name, double val) { (void)name, (void)val; }
void ___tracy_emit_plot_config(const char* name, int32_t type, int32_t step, int32_t fill, uint32_t color) {
    (void)name, (void)type, (void)step, (void)fill, (void)color;
    g_test.configs++;
}
int ___tracy_begin_sampling_profiling(void) {
    g_test.samplings++;
    return 0;
}
void ___tracy_end_sampling_profiling(void) {}
void ___tracy_emit_frame_mark(const char* name) {
    (void)name;
    g_test.frames++;
}
int snx_tracy_attached(void) { return g_test.connected; }
void ___tracy_emit_frame_mark_start(const char* name) { (void)name; }
void ___tracy_emit_frame_mark_end(const char* name) { (void)name; }
void ___tracy_emit_logString(int8_t severity, int32_t color, int32_t callstack_depth, size_t size, const char* txt) {
    (void)color, (void)callstack_depth;
    g_test.severity = severity;
    g_test.msgs++;
    g_test.msg_size = size;
    snprintf(g_test.msg, sizeof g_test.msg, "%.*s", (int)(size < sizeof g_test.msg - 1 ? size : sizeof g_test.msg - 1),
             txt);
}
uint64_t ___tracy_alloc_srcloc_name(uint32_t line, const char* source, size_t source_size, const char* function,
                                    size_t function_size, const char* name, size_t name_size, uint32_t color) {
    (void)line, (void)source, (void)source_size, (void)function, (void)function_size, (void)name, (void)name_size;
    (void)color;
    /* the client's own block for the name: a fresh one each call, as its malloc does */
    return (uint64_t)(++g_test.srclocs) + 1;
}
void ___tracy_emit_gpu_new_context_serial(const struct ___tracy_gpu_new_context_data d) {
    g_test.gpu_begins++; /* the context itself */
    g_test.gpu_time = d.gpuTime;
    g_test.gpu_period = d.period;
    g_test.gpu_ctx = d.context;
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
    g_test.gpu_syncs++;
    g_test.gpu_sync_time = d.gpuTime;
}
void ___tracy_emit_gpu_calibration_serial(const struct ___tracy_gpu_calibration_data d) {
    (void)d;
    g_test.gpu_calibrates++;
}
void ___tracy_fiber_enter(const char* fiber) {
    (void)fiber;
    g_test.fibers++;
}
void ___tracy_fiber_leave(void) { g_test.fiber_leaves++; }
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

/* A seeded run over the wrapper. The ops mix zones, fibers, plots, messages and a GUI that comes and goes. After
   each one the counts must still agree: an open zone holds one pending count, and a fiber stack cannot overflow. */
static uint32_t fuzz_next(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}
static void fuzz(uint32_t seed) {
    static const struct ___tracy_source_location_data where = {NULL, "fuzz", __FILE__, __LINE__, 0};
    TracyCZoneCtx zones[SNX_TRACY_ZONE_MAX];
    uint32_t s = seed ? seed : 1;
    int depth = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t op = fuzz_next(&s) % 12;
        if (op < 2) {
            if (depth < SNX_TRACY_ZONE_MAX) {
                TracyCZoneCtx c = snx_tracy_zone_begin(&where);
                if (c.active) zones[depth++] = c;
            }
        } else if (op < 4 && depth) {
            int k = (int)(fuzz_next(&s) % (uint32_t)depth);
            TracyCZoneCtx c = zones[k];
            memmove(&zones[k], &zones[k + 1], sizeof zones[0] * (size_t)(depth - k - 1));
            depth--;
            snx_tracy_zone_end(c);
        } else if (op < 6) {
            char name[32];
            snprintf(name, sizeof name, "wait %u", fuzz_next(&s) % 8);
            uint32_t slot = fuzz_next(&s) % SNX_FIBER_SLOTS; /* each draw goes into a local: the order is defined */
            uint32_t kind = fuzz_next(&s) % SNX_FIBER_KINDS;
            SNX_FIBER_ENTER(slot, kind, name);
        } else if (op == 6)
            SNX_FIBER_LEAVE();
        else if (op == 7)
            SNX_FIBER_LEAVE_ALL();
        else if (op == 8)
            g_test.connected = !g_test.connected;
        else if (op == 9)
            g_test.conn = fuzz_next(&s) % 4 + 1;
        else if (op == 10)
            SNX_PLOT("fuzz plot", (double)(fuzz_next(&s) % 100));
        else
            SNX_MSG("fuzz msg", 8);
        CHECK(depth >= 0 && depth <= SNX_TRACY_ZONE_MAX);
        CHECK(t_tracy.fiber_depth >= 0 && t_tracy.fiber_depth <= SNX_TRACY_FIBER_MAX);
        CHECK(g_tracy_pending == depth); /* every other emission balances its count inside the call */
    }
    SNX_FIBER_LEAVE_ALL();
    CHECK(t_tracy.fiber_depth == 0);
    while (depth) snx_tracy_zone_end(zones[--depth]);
    CHECK(g_tracy_pending == 0);
    g_test.connected = 1;
}

int main(void) {
    static const struct ___tracy_source_location_data where = {NULL, "zone", __FILE__, __LINE__, 0};
    TracyCZoneCtx ctx;
    int logs;
    unsigned dropped0;
    /* the settings gates are off before init reads the .ini: no client has applied them yet */
    CHECK(!atomic_load_explicit(&snx_tracy_memory_callstack, memory_order_relaxed) &&
          !atomic_load_explicit(&snx_tracy_lock_marks, memory_order_relaxed) &&
          !atomic_load_explicit(&snx_tracy_gpu_zone_callstack, memory_order_relaxed));
    /* a scoped section with no client is a no-op: reaching the next statement is the check that it did not crash */
    {
        SNX_SECTION_SCOPED(test_section, "test section");
    }
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
    g_settings.tracy_context_switches = 1; /* the sampler starts at the connect when this setting asks for it */
    ctx = snx_tracy_zone_begin(&where);
    CHECK(ctx.active && g_test.zones == 1 && g_test.samplings == 1);
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
        snx_tracy_gpu_cal_announce(&cal, 1, 1000, 10000);
        CHECK(cal.calibrated && cal.cpu_anchor == 1000 && cal.cpu_prev == 1000 && cal.gpu_prev == 10000);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1200, 10100) == 200 && cal.cpu_prev == 1200);
        g_test.conn = 3; /* a new connection: its server is back at the replayed announcement's pair */
        snx_tracy_gpu_cal_reconnect(&cal);
        CHECK(cal.cpu_prev == 1000);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1300, 10200) == 300 && cal.cpu_prev == 1300);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 1300, 10200) == 0 && cal.cpu_prev == 1300); /* a clock that stands still */
        snx_tracy_gpu_cal_reconnect(&cal); /* the same connection leaves the anchor where the last event put it */
        CHECK(cal.cpu_prev == 1300);
        /* an announcement without a clock pair latches uncalibrated: a delta would trip the server's assert */
        snx_tracy_gpu_cal_announce(&cal, 0, 0, 0);
        snx_tracy_gpu_cal_reconnect(&cal);
        CHECK(!cal.calibrated && cal.cpu_anchor == 0 && cal.cpu_prev == 0);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 5000, 6000) == 0 && cal.cpu_prev == 0);
        /* a calibrated context whose clock read 0 at the announcement starts at the next pair */
        snx_tracy_gpu_cal_announce(&cal, 1, 0, 0);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 700, 100) == 0 && cal.cpu_prev == 700);
        CHECK(snx_tracy_gpu_cal_delta(&cal, 900, 300) == 200);
    }
    /* ctx 2 is a real compute context: it has its own slot in the GPU arrays, announced once like any other, with
       its own period and a device offset it keeps for the emissions that follow */
    {
        int before = g_test.gpu_begins;
        int64_t offset;
        CHECK(snx_tracy_gpu_context(2, 5000, 2.0f, 1, "compute") == 1);
        CHECK(g_test.gpu_begins == before + 2); /* the announcement's context and its name */
        CHECK(g_test.gpu_ctx == 2 && g_test.gpu_period == 2.0f); /* ctx 2 sends its own period */
        offset = 5000 - g_test.gpu_time; /* the device offset the announcement took */
        CHECK(snx_tracy_gpu_context(2, 5000, 2.0f, 1, "compute") == 0);
        CHECK(g_test.gpu_begins == before + 2);
        snx_tracy_gpu_sync(6000, 2);
        CHECK(g_test.gpu_syncs == 2); /* the ctx 0 anchor above, and this one */
        CHECK(6000 - g_test.gpu_sync_time == offset); /* a later device time keeps the same offset: it is taken once */
    }
    /* a thread's first unnamed mark on a connection is a seed; the next one emits. each mark is a per-thread
       seed, so two concurrent threads do not change the count */
    {
        int frames = g_test.frames;
        pthread_t t[2];
        g_test.conn = 4;
        for (int i = 0; i < 2; i++) CHECK(!pthread_create(&t[i], NULL, frame_mark_seed, NULL));
        for (int i = 0; i < 2; i++) CHECK(!pthread_join(t[i], NULL));
        CHECK(g_test.frames == frames && g_tracy_pending == 0); /* both threads' first marks on conn 4 were seeds */
        snx_tracy_frame_mark(); /* this thread's first mark on conn 4 is a seed too */
        CHECK(g_test.frames == frames && g_tracy_pending == 0);
        snx_tracy_frame_mark();
        CHECK(g_test.frames == frames + 1 && g_tracy_pending == 0);
        g_test.conn = 5;
        for (int i = 0; i < 2; i++) CHECK(!pthread_create(&t[i], NULL, frame_mark_seed, NULL));
        for (int i = 0; i < 2; i++) CHECK(!pthread_join(t[i], NULL));
        /* fresh host threads on the new connection skip their first mark too */
        CHECK(g_test.frames == frames + 1 && g_tracy_pending == 0);
    }
    /* a named set has its own seed per name and per connection, and every path releases the emission count */
    {
        int frames = g_test.frames;
        g_test.conn = 7;
        snx_tracy_frame_mark_named("a set");
        CHECK(g_test.frames == frames && g_tracy_pending == 0); /* the first mark seeds the set */
        snx_tracy_frame_mark_named("a set");
        CHECK(g_test.frames == frames + 1 && g_tracy_pending == 0);
        snx_tracy_frame_mark_named("another set");
        CHECK(g_test.frames == frames + 1 && g_tracy_pending == 0); /* a new name seeds its own set */
        g_test.conn = 8;
        snx_tracy_frame_mark_named("a set");
        CHECK(g_test.frames == frames + 1 && g_tracy_pending == 0); /* a new connection re-seeds a known name */
        snx_tracy_frame_mark_named("a set");
        CHECK(g_test.frames == frames + 2 && g_tracy_pending == 0);
    }
    /* SNX_PLOT_CFG evaluates its name once and sends the format once per name and connection. One host thread on
       two connections: the name's side effect counts the evaluations, and the new connection sends the format again. */
    {
        pthread_t t;
        g_test.conn = 6;
        g_test.configs = 0;
        g_plot_name_evals = 0;
        CHECK(!pthread_create(&t, NULL, plot_cfg_thread, NULL));
        CHECK(!pthread_join(t, NULL));
        CHECK(g_plot_name_evals == 2 && g_test.configs == 1);
        g_test.conn = 7;
        CHECK(!pthread_create(&t, NULL, plot_cfg_thread, NULL));
        CHECK(!pthread_join(t, NULL));
        CHECK(g_plot_name_evals == 4 && g_test.configs == 2); /* the new connection sent the format again */
    }
    /* a callstack zone goes through the callstack begin with its depth, and ends at the block's scope */
    {
        g_test.callstacks = 0;
        SNX_ZONE_CS(cs, "callstack zone", 2);
        CHECK(cs.active && g_tracy_pending == 1);
        CHECK(g_test.callstacks == 1 && g_test.callstack_depth == 2);
    }
    CHECK(g_tracy_pending == 0); /* the scope's cleanup ended the callstack zone */
    /* a depth of 0 takes the setting's; one past the server's 16 is clamped, and no depth sends a plain zone */
    atomic_store_explicit(&snx_tracy_zone_callstack, 3, memory_order_relaxed);
    ctx = snx_tracy_zone_begin_cs(&where, 0);
    CHECK(ctx.active && g_test.callstacks == 2 && g_test.callstack_depth == 3);
    snx_tracy_zone_end(ctx);
    ctx = snx_tracy_zone_begin_cs(&where, 40);
    CHECK(ctx.active && g_test.callstacks == 3 && g_test.callstack_depth == 16);
    snx_tracy_zone_end(ctx);
    atomic_store_explicit(&snx_tracy_zone_callstack, 0, memory_order_relaxed);
    ctx = snx_tracy_zone_begin_cs(&where, 0);
    CHECK(ctx.active && g_test.callstacks == 3); /* no depth: a plain zone */
    snx_tracy_zone_end(ctx);
    /* The four new trace parameters write these atomics from the C++ callback while the readers here run without a
       lock. A GPU depth above zero switches the begin to the callstack event; lock marks are a boolean gate. */
    {
        int before = g_test.gpu_callstacks;
        atomic_store_explicit(&snx_tracy_gpu_zone_callstack, 4, memory_order_relaxed);
        CHECK(atomic_load_explicit(&snx_tracy_gpu_zone_callstack, memory_order_relaxed) == 4);
        snx_tracy_gpu_zone_begin("gpu pass", 0, 2, NULL); /* ctx 2 was announced above */
        CHECK(g_test.gpu_callstacks == before + 1);
        atomic_store_explicit(&snx_tracy_gpu_zone_callstack, 0, memory_order_relaxed);
        atomic_store_explicit(&snx_tracy_lock_marks, 1, memory_order_relaxed);
        CHECK(atomic_load_explicit(&snx_tracy_lock_marks, memory_order_relaxed) == 1);
        atomic_store_explicit(&snx_tracy_lock_marks, 0, memory_order_relaxed);
    }
    /* A transient zone copies its expression name into an srcloc the client owns. The scope cleanup ends the zone and
       leaves no pending count; a 0 depth takes the setting, and a depth past 16 is clamped. */
    {
        int srclocs = g_test.srclocs;
        g_test.transient_begins = g_test.transient_callstacks = 0;
        {
            SNX_ZONE_TRANSIENT(tz, "a transient zone");
            CHECK(tz.active && g_tracy_pending == 1 && g_test.transient_begins == 1);
            CHECK(g_test.srclocs == srclocs + 1); /* one client-owned srcloc for the name */
        }
        CHECK(g_tracy_pending == 0);
        {
            SNX_ZONE_TRANSIENT_CS(tz, "a transient callstack zone", 40);
            CHECK(tz.active && g_tracy_pending == 1 && g_test.transient_callstacks == 1);
            CHECK(g_test.callstack_depth == 16);
        }
        CHECK(g_tracy_pending == 0);
    }
    dropped0 = g_tracy_dropped; /* drops before this test's deep zones, as a baseline */
    /* a message: one emission, gated as the other short ones are, and nothing after the gate shuts */
    g_test.msgs = 0;
    SNX_MSG("a syncpoint wait", 16);
    CHECK(g_test.msgs == 1 && g_test.msg_size == 16 && !strncmp(g_test.msg, "a syncpoint wait", 16));
    snx_tracy_fatal("a fatal end");
    CHECK(g_test.msgs == 2 && !strncmp(g_test.msg, "a fatal end", 11));
    CHECK(g_test.severity == TracyMessageSeverityFatal); /* the message pane marks it as fatal */
    /* the severity lookup clamps an out-of-range value to the trace and fatal ends */
    CHECK(snx_tracy_severity(-1) == TracyMessageSeverityTrace);
    CHECK(snx_tracy_severity(0) == TracyMessageSeverityTrace);
    CHECK(snx_tracy_severity(5) == TracyMessageSeverityFatal);
    CHECK(snx_tracy_severity(6) == TracyMessageSeverityFatal);
    /* the guest's waits: a fiber's name goes out once per enter, and an unpaired leave is dropped */
    g_test.fibers = g_test.fiber_leaves = 0;
    SNX_FIBER_LEAVE();
    CHECK(g_test.fiber_leaves == 0);
    SNX_FIBER_ENTER(0, 1, "waiting on an event");
    CHECK(g_test.fibers == 1 && t_tracy.fiber_depth == 1);
    SNX_FIBER_ENTER(0, 2, "the game is paused"); /* a nested wait, which the stack must pair in reverse */
    CHECK(g_test.fibers == 2 && t_tracy.fiber_depth == 2);
    SNX_FIBER_LEAVE_ALL();
    CHECK(g_test.fiber_leaves == 2 && t_tracy.fiber_depth == 0);
    /* a name lives at one address for the server, which reads it when a GUI asks; the table keeps it */
    CHECK(!strcmp(g_fiber_names[0][1], "waiting on an event"));
    /* a GUI that disconnects mid-wait must not leave a depth that leave_all cannot clear */
    g_test.connected = 1;
    SNX_FIBER_ENTER(0, 1, "waiting on an event");
    SNX_FIBER_ENTER(0, 2, "the game is paused");
    g_test.connected = 0;
    SNX_FIBER_LEAVE_ALL();
    CHECK(t_tracy.fiber_depth == 0);
    g_test.connected = 1;
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
    /* stress the wrapper with seeded runs, then restore the counts the shutdown checks below read */
    {
        int zone_ends = g_test.zone_ends, msgs = g_test.msgs;
        for (uint32_t seed = 1; seed <= 32; seed++) fuzz(seed);
        g_test.zone_ends = zone_ends;
        g_test.msgs = msgs;
    }
    /* shut down with a zone open: the zone ended once, the gate shut, the client shut down once */
    ctx = snx_tracy_zone_begin(&where);
    CHECK(ctx.active && g_tracy_pending == 1);
    snx_tracy_shutdown();
    CHECK(g_test.shutdowns == 1 && g_test.zone_ends == 67 && g_tracy_pending == 0); /* the deep test's, and this one's */
    snx_tracy_zone_end(ctx);
    CHECK(g_test.zone_ends == 67);
    SNX_MSG("after the stop", 14);
    CHECK(g_test.msgs == 2); /* the gate is shut: the client's data cannot be read */
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
