/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Tracy client lifetime and emission gate; the client is manual-lifetime. */
#include "common/utilities/tracy.h"
#include "common/platform/dl.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#if defined(SNX_TRACY)
#include <SDL3/SDL_process.h>
/* g_tracy_state is the client's, and the emissions' gate is it being SNX_TRACY_ON; g_tracy_pending counts them in
   flight. t_tracy is this thread's: the zones it has open, its zone tick, whether it mirrors a connection, the
   connection its frame mark was seeded on, the name and group hint it goes by, and the nested wait-fiber stack. */
#define SNX_TRACY_ZONE_MAX  64
#define SNX_TRACY_PLOT_CFG_MAX 64 /* snx_tracy_plot_cfg's name cache, not tracy_plots_send's fixed table */
#define SNX_TRACY_FIBER_MAX  4 /* room for a wait nested inside another, tracked as a stack per thread */
#define SNX_TRACY_POLL      64 /* a low-rate thread must not stay on a dead connection for thousands of zones */
#define SNX_TRACY_OFF     0 /* not started */
#define SNX_TRACY_STARTING 1 /* starting the client: the gate stays shut */
#define SNX_TRACY_ON      2
#define SNX_TRACY_STOPPED 3 /* shut down, for good */
#define SNX_TRACY_DONE     4 /* capture finished; a late caller can exit */
#define SNX_TRACY_FIBER_NAME_MAX 96
#define SNX_TRACY_FRAME_NAME_MAX 64
#define SNX_TRACY_FRAME_NAME_LEN 96
#define SNX_TRACY_GPU_CTX 8
#define SNX_TRACY_LOCK_MARK_MAX 64
/* One state struct per file: the mutable globals below are its members. The macros keep the call sites short. */
typedef struct {
    _Atomic int state;
    _Atomic int pending;
    _Atomic uint32_t dropped;
    char fiber_names[SNX_FIBER_SLOTS][SNX_FIBER_KINDS][SNX_TRACY_FIBER_NAME_MAX];
    struct {
        PMutex lock;
        struct {
            char name[SNX_TRACY_FRAME_NAME_LEN];
            uint64_t conn;
        } slot[SNX_TRACY_FRAME_NAME_MAX];
        int n;
    } frame_names;
    SDL_Process* capture;
    char capture_path[SNX_PATH_SIZE + 64];
    char title[256];
    /* the runtime's (SnxTracyHost): its folder and name, kept, its flags and its log */
    char dir[SNX_PATH_SIZE], name[256];
    int capture_asked, client_asked, headless;
    void (*log)(int warn, const char* line);
    char port[16];
    PMutex capture_lock;
    PMutex wait_lock;
    PCond wait_cond;
    _Atomic int sampling;
    PMutex plot_cfg_lock;
    struct {
        const char* name;
        uint64_t conn;
    } plot_cfg[SNX_TRACY_PLOT_CFG_MAX];
    int plot_cfg_n;
    int gpu_sent[SNX_TRACY_GPU_CTX];
    uint16_t gpu_query[SNX_TRACY_GPU_CTX];
    int64_t gpu_offset[SNX_TRACY_GPU_CTX];
    float gpu_period[SNX_TRACY_GPU_CTX];
    uint8_t gpu_anchored[SNX_TRACY_GPU_CTX];
    uint64_t start_ns;
    struct {
        PMutex lock;
        struct {
            const char* file;
            uint32_t line;
            struct ___tracy_source_location_data srcloc;
        } slot[SNX_TRACY_LOCK_MARK_MAX];
        int n;
    } lock_marks;
} SnxTracyState;
static SnxTracyState g_tracy;
/* a line of the runtime's log, its end with it, through the host's log (snx_tracy_init) or stderr */
[[gnu::format(printf, 2, 3)]] static void tracy_say(int warn, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_tracy.log)
        g_tracy.log(warn, line);
    else
        fputs(line, stderr);
}
#define LOG(...)        tracy_say(0, __VA_ARGS__)
#define WARN(...)       tracy_say(1, __VA_ARGS__)
#define g_tracy_state  (g_tracy.state)
#define g_tracy_pending (g_tracy.pending)
#define g_tracy_dropped (g_tracy.dropped)
typedef struct {
    TracyCZoneCtx zones[SNX_TRACY_ZONE_MAX];
    int zone_depth;
    int attached;
    int fiber_depth;
    uint8_t fiber_slot[SNX_TRACY_FIBER_MAX];
    uint8_t fiber_kind[SNX_TRACY_FIBER_MAX];
    uint64_t fiber_conn[SNX_TRACY_FIBER_MAX]; /* the connection each fiber's enter went out on */
    unsigned zone_tick;
    uint64_t conn;       /* the connection attached mirrors: a new one is re-announced to its server */
    uint64_t frame_conn; /* the connection the first unnamed frame mark was a seed for */
    char name[64];
    char hint[64]; /* the group hint the C++ bridge replays with the name at each connection */
} TracyThread;
static _Thread_local TracyThread t_tracy;

/* The server keys a fiber by its name's address and reads the string when a GUI asks, so the name must stay at one
   address for the capture's life. One slot of each kind gives every guest thread's wait its own lane. */
#define SNX_TRACY_FIBER_NAME_MAX 96
#define g_fiber_names (g_tracy.fiber_names)

/* A named frame mark goes out as the name's address, which the server reads when a GUI asks, so each name lives here
   for the capture's life. The table also holds the connection whose first mark for the name was only a seed. The
   mutex makes the name and seed one step for threads that mark the same set. */
#define SNX_TRACY_FRAME_NAME_MAX 64
#define SNX_TRACY_FRAME_NAME_LEN 96
#define g_tracy_frame_names (g_tracy.frame_names)

/* The launcher's tracy-capture process; it writes the trace when the client disconnects. */
#define g_tracy_capture      (g_tracy.capture)
#define g_tracy_capture_path (g_tracy.capture_path)
#define g_tracy_title        (g_tracy.title)
#define g_tracy_port         (g_tracy.port)

/* These gates start at the settings' values. The parameter callback changes each while a server is attached, and a
   slow frame can raise the callstack depths for a few frames; the allocator, the zone begins, the renderer and the
   lock call sites read them on other threads, so each gate is atomic and each read is relaxed. */
_Atomic int snx_tracy_frame_images_live;
_Atomic int snx_tracy_memory_detail;
_Atomic int snx_tracy_memory_detail_ever;
_Atomic int snx_tracy_memory_callstack;
_Atomic int snx_tracy_zone_callstack;
_Atomic int snx_tracy_gpu_zone_callstack;
_Atomic int snx_tracy_lock_marks;

/* One capture at a time; shutdown may lock from another thread. */
#define g_tracy_capture_lock (g_tracy.capture_lock)
static void tracy_capture_lock(void) { pm_lock(&g_tracy_capture_lock); }
static void tracy_capture_unlock(void) { pm_unlock(&g_tracy_capture_lock); }

/* For what only the client or the capture can change: Tracy has no event for a connection or a capture's exit,
   so those are waited for here with a timeout, and every emission that finds no more in flight wakes shutdown. */
#define g_tracy_wait_lock (g_tracy.wait_lock)
#define g_tracy_wait_cond (g_tracy.wait_cond)
static void tracy_wait_ns(int64_t ns) {
    pm_lock(&g_tracy_wait_lock);
    pc_wait(&g_tracy_wait_cond, &g_tracy_wait_lock, ns);
    pm_unlock(&g_tracy_wait_lock);
}

/* the plots' formats, once a GUI attaches */
static void tracy_plots_send(void);

/* The sampler starts when the run asks for it, tracy_sampling, at the connect. Its samples land on the timeline as
   gray sample zones, which with JIT'd guest code and the drivers' own threads (the NVIDIA helpers', MangoHud's) reads
   as a wall of [unknown]; the instrumented zones alone don't, so the sampler is off unless the trace wants the flame
   graphs. */
#define g_tracy_sampling (g_tracy.sampling)
static void tracy_sampling_start(void) {
    /* Tracy 0.15.0 starts its system trace (the OS's context switches, and the sampled stacks with it) from
       BeginSamplingProfiling, which this build makes manual. Both settings ask for that one call; without it the
       context-switch lanes stay empty even though the library can capture them. */
    if (!g_settings.tracy_sampling && !g_settings.tracy_context_switches) return;
    if (atomic_exchange_explicit(&g_tracy_sampling, 1, memory_order_acq_rel)) return;
    if (TracyCBeginSamplingProfiling()) LOG("tracy: the stack sampler and the OS's context switches are on\n");
}
static void tracy_sampling_stop(void) {
    if (!atomic_exchange_explicit(&g_tracy_sampling, 0, memory_order_acq_rel)) return;
    TracyCEndSamplingProfiling();
}

/* A tool of Tracy's: beside the runtime first (the build it was made with), then PATH. */
static char* tracy_tool(const char* name) {
    char* beside = snx_path_join(g_tracy.dir[0] ? g_tracy.dir : ".", name);
    char* tool = beside ? snx_find_tool(beside) : NULL;
    free(beside);
    return tool ? tool : snx_find_tool(name);
}

/* The client listens on the port it reads as it starts: the setting's, or, when a capture is asked for, one of the
   run's own so an earlier capture (or a GUI) on 8086 is not in the way. Without either the client keeps Tracy's 8086.
   Tracy's client still reads TRACY_PORT from the environment, so that one variable is set for it; nothing here reads
   the environment back. */
static void tracy_port_setup(int capture) {
    const char* port = g_settings.tracy_port;
    if (g_tracy_port[0]) return;
    if (port[0]) {
        /* A port Tracy's client can bind: it takes the setting as it is, and a value that is not a number makes
           it exit and leave the capture connecting to nothing, so it is checked here rather than passed on. */
        char* end;
        unsigned long n;
        size_t i;
        for (i = 0; port[i]; i++)
            if (port[i] < '0' || port[i] > '9') break;
        n = strtoul(port, &end, 10);
        if (*port && !port[i] && !*end && end != port && n > 0 && n < 65536)
            snprintf(g_tracy_port, sizeof g_tracy_port, "%lu", n);
        else
            WARN("tracy: Tracy port %s is not a port number\n", port);
    }
    if (!g_tracy_port[0]) {
        /* none named, or an unusable one: only a capture needs a port of its own, and without one the client
           keeps Tracy's 8086 */
        if (!capture) return;
        snprintf(g_tracy_port, sizeof g_tracy_port, "%d", 18086 + (int)(plat_tid() % 1000));
    }
    if (plat_setenv("TRACY_PORT", g_tracy_port)) WARN("tracy: cannot set the client's port %s\n", g_tracy_port);
}

static int tracy_capture_start(int seconds, int launch) {
    char dir[SNX_PATH_SIZE + 16], seconds_text[16], *tool;
    const char* named = g_settings.tracy_capture_tool;
    const char* argv[9];
    const char* suffix = launch ? "; the run was not started" : "";
    int n = 0;
    struct tm t;
    time_t now;
    snprintf(dir, sizeof dir, "%scaptures", g_tracy.dir);
    tool = named[0] ? snx_find_tool(named) : tracy_tool("tracy-capture");
    if (!tool) {
        WARN("tracy: %s needs tracy-capture " SNX_TRACY_VERSION " beside the runtime or on PATH%s\n",
             launch ? "--tracy-capture" : "a capture", suffix);
        return 0;
    }
    if (snx_mkdirs(dir)) {
        WARN("tracy: cannot make the capture folder %s%s\n", dir, suffix);
        free(tool);
        return 0;
    }
    now = time(NULL);
    memset(&t, 0, sizeof t);
#ifdef _WIN32
    if (localtime_s(&t, &now)) t.tm_year = 70;
#else
    if (!localtime_r(&now, &t)) t.tm_year = 70;
#endif
    snprintf(g_tracy_capture_path, sizeof g_tracy_capture_path, "%s/tracy_%s_%04d%02d%02d_%02d%02d%02d.tracy", dir,
             g_tracy.name, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    snx_path_native(g_tracy_capture_path);
    argv[n++] = tool;
    argv[n++] = "-o";
    argv[n++] = g_tracy_capture_path;
    argv[n++] = "-f";
    argv[n++] = "-s";
    snprintf(seconds_text, sizeof seconds_text, "%d", seconds);
    argv[n++] = seconds_text;
    if (g_tracy_port[0]) {
        argv[n++] = "-p";
        argv[n++] = g_tracy_port;
    }
    argv[n] = NULL;
    g_tracy_capture = SDL_CreateProcess(argv, false);
    free(tool);
    if (!g_tracy_capture) {
        WARN("tracy: cannot start tracy-capture: %s%s\n", SDL_GetError(), suffix);
        g_tracy_capture_path[0] = 0;
        return 0;
    }
    LOG("tracy: recording to %s\n", g_tracy_capture_path);
    return 1;
}

/* the trace opens in the profiler by itself, so that a run needs nothing but starting it */
static void tracy_open_trace(const char* path) {
    char* tool;
    const char* argv[3];
    if (!path || !*path || !g_settings.tracy_open_trace) return;
    if (g_tracy.headless) return; /* no screen to open it on */
    tool = tracy_tool("tracy-profiler");
    if (!tool) return;
    argv[0] = tool;
    argv[1] = path;
    argv[2] = NULL;
    if (!SDL_CreateProcess(argv, false)) {
        WARN("tracy: cannot open the trace in the profiler: %s\n", SDL_GetError());
        free(tool);
        return;
    }
    free(tool);
    LOG("tracy: the trace opens in the profiler\n");
}

static void tracy_capture_finish_locked(int clean) {
    int status = 0;
    bool done;
    uint64_t until = plat_ns() + 5000000000ull;
    if (!g_tracy_capture) return;
    /* A clean exit waits for the trace to be written; a killed one only looks once. */
    while (!(done = SDL_WaitProcess(g_tracy_capture, false, &status)) && clean && plat_ns() < until)
        tracy_wait_ns(10000000);
    if (done) {
        if (status == 0) {
            LOG("tracy: capture saved to %s\n", g_tracy_capture_path);
            tracy_open_trace(g_tracy_capture_path);
        } else
            WARN("tracy: capture did not save: %s (status %d)\n", g_tracy_capture_path, status);
        SDL_DestroyProcess(g_tracy_capture);
    } else {
        /* tracy-capture writes after the connection closes; killing it would lose the trace. */
        LOG("tracy: capture still writing to %s. It will finish after the runtime exits.\n", g_tracy_capture_path);
        SDL_DestroyProcess(g_tracy_capture);
    }
    g_tracy_capture = NULL;
    g_tracy_capture_path[0] = 0;
}

static void tracy_capture_finish(int clean) {
    tracy_capture_lock();
    tracy_capture_finish_locked(clean);
    tracy_capture_unlock();
}

/* host.c's reporters, in the client's terms. The admission (who reports on a lock) is host.c's, which owns the
   mutex. Above tracy_start_client, the one place that registers them, so F10's client registers as the first
   did: without it the locks pm_lock_announce counted report nothing for that trace. */
static void* pm_tracy_make(const char* name) { return snx_tracy_lock_make(name); }
static void pm_tracy_free(void* l) { snx_tracy_lock_free(l); }
static void pm_tracy_wait(void* l) { snx_tracy_lock_wait(l); }
static void pm_tracy_lock(void* l) { snx_tracy_lock_lock(l); }
static void pm_tracy_unlock(void* l) { snx_tracy_lock_unlock(l); }
static void pm_tracy_try(void* l, int acquired) { snx_tracy_lock_try(l, acquired); }
static const PmTracy g_pm_tracy_snx = {pm_tracy_make, pm_tracy_free,   pm_tracy_wait,
                                       pm_tracy_lock, pm_tracy_unlock, pm_tracy_try};

static void tracy_start_client(void) {
    char info[512];
    int n;
    int expected = SNX_TRACY_OFF;
    if (!atomic_compare_exchange_strong_explicit(&g_tracy_state, &expected, SNX_TRACY_STARTING, memory_order_seq_cst,
                                                 memory_order_acquire))
        return;
    /* No emission can reach the table while the gate is closed; the name table starts fresh with the client. */
    g_tracy_frame_names.n = 0;
    ___tracy_startup_profiler();
    /* The client exists now. Open the gate and hold one pending count across the whole startup tail so a
       concurrent shutdown cannot close the client between two of the registrations below. */
    atomic_fetch_add_explicit(&g_tracy_pending, 1, memory_order_seq_cst);
    atomic_store_explicit(&g_tracy_state, SNX_TRACY_ON, memory_order_seq_cst);
    /* The C++ side's program name and its parameters are registered on the new client. A client started later with
       F10 gets the same registrations as the first one. */
    snx_tracy_conn_set_program_name(g_tracy_title);
    snx_tracy_conn_register_parameters();
    snx_tracy_conn_register_source_callback();
    /* The categories the section scopes name; 0 needs no name, it is the uncategorized track. */
    snx_tracy_section_setup(1, "game");
    snx_tracy_section_setup(2, "shader");
    snx_tracy_section_setup(3, "save");
    snx_tracy_section_setup(4, "audio");
    snx_tracy_section_setup(5, "video");
    snx_tracy_section_setup(6, "frame");
    snx_tracy_section_setup(7, "vk");
    snx_tracy_section_setup(8, "rt");
    pm_tracy_register(&g_pm_tracy_snx); /* safe again: each lock keeps its single context */
    n = snprintf(info, sizeof info, "%s\n%s", snx_build_info(), g_tracy_title);
    atomic_fetch_add_explicit(&g_tracy_pending, 1, memory_order_acq_rel);
    snx_tracy_thread_name("main");
    if (n > 0) TracyCAppInfo(info, (size_t)(n < (int)sizeof info ? n : (int)sizeof info - 1));
    snx_tracy_leave(); /* the thread-name count: broadcasting at zero wakes a waiting shutdown */
    snx_tracy_leave(); /* the startup count, after all registrations */
    if (g_tracy.capture_asked)
        LOG("tracy: profiling this run to its capture\n");
    else
        LOG("tracy: profiling this run. Connect the GUI with: tracy-profiler -a 127.0.0.1 -p %s\n",
            g_tracy_port[0] ? g_tracy_port : "8086");
}

/* Starts a capture and waits for it to connect. The capture's handshake has a couple of seconds to be answered, and
   a machine busy with a recompile can miss it: the capture gives up then, and another one takes its place. */
static int tracy_capture_wait(void) {
    int i, j;
    for (i = 0; i < 3; i++) {
        int status = 0;
        int gave_up = 0;
        if (!tracy_capture_start(g_settings.tracy_capture_seconds, 1)) return 0;
        for (j = 0; j < 200; j++) {
            if (TracyCIsConnected) return 1;
            if (SDL_WaitProcess(g_tracy_capture, false, &status)) {
                gave_up = 1; /* it gave up */
                break;
            }
            tracy_wait_ns(50000000);
        }
        if (TracyCIsConnected) return 1;
        if (!gave_up) {
            /* It still holds the port, so another capture could not bind it. This one may still connect. */
            WARN("tracy: the capture did not connect in 10 s and is still running. It may connect yet.\n");
            return 0;
        }
        SDL_DestroyProcess(g_tracy_capture);
        g_tracy_capture = NULL;
        g_tracy_capture_path[0] = 0;
        LOG("tracy: the capture did not connect. Another one takes its place.\n");
    }
    return 0;
}

/* a capture that is already running: a moment for it to connect, and the game waits that long */
static void tracy_capture_connected(void) {
    int i;
    for (i = 0; i < 20 && !TracyCIsConnected; i++) tracy_wait_ns(50000000);
    if (!TracyCIsConnected) LOG("tracy: the capture did not connect\n");
}

void snx_tracy_capture_now(void) {
    int status = 0, state;
    tracy_capture_lock();
    state = atomic_load_explicit(&g_tracy_state, memory_order_acquire);
    if (state == SNX_TRACY_STARTING) {
        while (atomic_load_explicit(&g_tracy_state, memory_order_acquire) == SNX_TRACY_STARTING)
            tracy_wait_ns(1000000);
        state = atomic_load_explicit(&g_tracy_state, memory_order_acquire);
    }
    if (state == SNX_TRACY_STOPPED || state == SNX_TRACY_DONE) goto out;
    if (state == SNX_TRACY_OFF) {
        tracy_port_setup(1); /* before the client reads it, which is when it starts */
        tracy_start_client();
        if (!tracy_capture_start(g_settings.tracy_capture_seconds, 0)) goto out;
        tracy_capture_connected(); /* the game waits that long for it */
    } else {
        if (g_tracy_capture) {
            if (!SDL_WaitProcess(g_tracy_capture, false, &status)) {
                LOG("tracy: a capture is already recording to %s\n", g_tracy_capture_path);
                goto out;
            }
            tracy_capture_finish_locked(1);
        }
        if (TracyCIsConnected) {
            LOG("tracy: the Tracy GUI is connected. Disconnect it before starting a capture.\n");
            goto out;
        }
        if (!tracy_capture_start(g_settings.tracy_capture_seconds, 0)) goto out;
    }
    LOG("tracy: capture started. It stops after %d seconds.\n", g_settings.tracy_capture_seconds);
out:
    tracy_capture_unlock();
}

/* 1 while the client's emissions are allowed: closed before it starts and, again, before it shuts down. tracy_conn
   reads it so TracyCIsConnected is never reached while the client has no data to read (an assert). */
int snx_tracy_gate_open(void) { return atomic_load_explicit(&g_tracy_state, memory_order_acquire) == SNX_TRACY_ON; }

int snx_tracy_enter(void) {
    /* The count comes before the second gate read, and both are sequenced with the compare-exchange shutdown closes it with,
       so a thread that is about to emit is either counted before shutdown looks or sees the gate closed. */
    if (atomic_load_explicit(&g_tracy_state, memory_order_acquire) != SNX_TRACY_ON) return 0;
    /* This increment is the shutdown handshake, not a measured cost: it lets shutdown wait for the emissions in
       flight. Keep it on every emission — removing or moving it needs timing of a real capture first. */
    atomic_fetch_add_explicit(&g_tracy_pending, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_tracy_state, memory_order_seq_cst) != SNX_TRACY_ON) {
        snx_tracy_leave();
        return 0;
    }
    return 1;
}

void snx_tracy_leave(void) {
    /* Whenever the count reaches zero, wake whoever waits on it, whether or not shutdown has marked itself: a
       thread may read that mark before shutdown's store is visible and so not wake a waiter already blocking. */
    if ((atomic_fetch_sub_explicit(&g_tracy_pending, 1, memory_order_acq_rel) - 1) == 0) {
        pm_lock(&g_tracy_wait_lock);
        pc_broadcast(&g_tracy_wait_cond);
        pm_unlock(&g_tracy_wait_lock);
    }
}

/* A connection this thread has not yet announced to: its server knows neither this thread's name, nor the plots'
   formats, nor that the sampler should run. Called on the first zone after a connect, and again on a new one. */
static void tracy_announce(void) {
    if (t_tracy.name[0]) {
        /* A hinted name goes through the C++ bridge, which applies the hint the way this client's Tracy does. */
        if (t_tracy.hint[0])
            snx_tracy_conn_thread_name_hint(t_tracy.name, t_tracy.hint);
        else
            TracyCSetThreadName(t_tracy.name);
    }
    tracy_sampling_start();
    tracy_plots_send();
}

/* The two begins share the gate, the connection poll and the 64-slot depth table: a callstack boolean of 0 is a plain
   zone, and a set boolean sends depth through ___tracy_emit_zone_begin_callstack. */
static TracyCZoneCtx tracy_zone_begin(const struct ___tracy_source_location_data* srcloc, int callstack, int depth) {
    TracyCZoneCtx ctx = {0};
    uint64_t conn;
    if (atomic_load_explicit(&g_tracy_state, memory_order_acquire) != SNX_TRACY_ON) return ctx;
    /* No room to track a deeper zone: drop it rather than leak a begin. */
    if (t_tracy.zone_depth >= SNX_TRACY_ZONE_MAX) {
        atomic_fetch_add_explicit(&g_tracy_dropped, 1, memory_order_relaxed);
        return ctx;
    }
    /* A thread not mirroring a connection asks at every zone, so one that makes few — the frontend's one a frame,
       the audio clock's one a tick — is traced from its next one. Attached, it only looks again every so many
       zones, for a disconnection. */
    if (!t_tracy.attached || ++t_tracy.zone_tick >= SNX_TRACY_POLL) {
        t_tracy.zone_tick = 0;
        if (!snx_tracy_enter()) return ctx;
        t_tracy.attached = ___tracy_connected();
        conn = snx_tracy_connection_id();
        /* A disconnect and a reconnect between two polls leave attached as it was, so without this the new
           server would never be told any of it. */
        if (t_tracy.attached && conn != t_tracy.conn) {
            t_tracy.conn = conn;
            tracy_announce();
        }
        /* the dropped count uses the number format; SNX_PLOT_CFG sends that format once per name and connection */
        SNX_PLOT_CFG("tracy dropped zones", (double)atomic_load_explicit(&g_tracy_dropped, memory_order_relaxed), 0, 0,
                     1, 0);
        snx_tracy_leave();
        if (!t_tracy.attached) return ctx;
    }
    /* A low-rate thread must not keep announcing to a connection that went away and came back. Compare the id on
       every zone without doing the whole poll's work. */
    if (t_tracy.attached && snx_tracy_enter()) {
        uint64_t conn = snx_tracy_connection_id();
        if (conn != t_tracy.conn) {
            t_tracy.conn = conn;
            tracy_announce();
        }
        snx_tracy_leave();
    }
    /* The count brackets the emit, so shutdown cannot end the client under it. */
    if (!snx_tracy_enter()) return ctx;
    /* a plain zone takes no callstack: the sampler's stacks are for that; SNX_ZONE_CS asks for one */
    ctx = callstack ? ___tracy_emit_zone_begin_callstack(srcloc, depth, 1) : ___tracy_emit_zone_begin(srcloc, 1);
    if (ctx.active)
        t_tracy.zones[t_tracy.zone_depth++] = ctx;
    else
        snx_tracy_leave();
    return ctx;
}

TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc) {
    return tracy_zone_begin(srcloc, 0, 0);
}

TracyCZoneCtx snx_tracy_zone_begin_cs(const struct ___tracy_source_location_data* srcloc, int depth) {
    if (depth <= 0) depth = atomic_load_explicit(&snx_tracy_zone_callstack, memory_order_relaxed);
    if (depth <= 0) return tracy_zone_begin(srcloc, 0, 0);
    if (depth > 16) depth = 16;
    return tracy_zone_begin(srcloc, 1, depth);
}

/* A transient zone carries a name that is only valid at the begin. tracy.c copies it into an srcloc the client owns:
   the client sends the payload and frees that block itself (and frees it immediately when the begin is inactive), so
   the end has no srcloc to release — it only ends the zone, as any other zone end does. */
TracyCZoneCtx snx_tracy_zone_begin_transient(const char* name, int callstack, int depth) {
    TracyCZoneCtx ctx = {0};
    uint64_t srcloc;
    if (!name || !*name || !snx_tracy_enter()) return ctx;
    /* No room to track a deeper zone: drop it rather than leak a begin. */
    if (t_tracy.zone_depth >= SNX_TRACY_ZONE_MAX) {
        atomic_fetch_add_explicit(&g_tracy_dropped, 1, memory_order_relaxed);
        snx_tracy_leave();
        return ctx;
    }
    if (!TracyCIsConnected) {
        snx_tracy_leave();
        return ctx;
    }
    /* A callstack with no explicit depth takes the setting, as SNX_ZONE_CS's begin does; 0 means a plain zone and the
       server's limit is 16 frames. */
    if (!callstack)
        depth = 0;
    else if (depth <= 0)
        depth = atomic_load_explicit(&snx_tracy_zone_callstack, memory_order_relaxed);
    if (depth > 16) depth = 16;
    srcloc = ___tracy_alloc_srcloc_name(0, __FILE__, sizeof __FILE__ - 1, __func__, sizeof __func__ - 1, name,
                                        strlen(name), 0);
    if (!srcloc) {
        snx_tracy_leave();
        return ctx;
    }
    ctx = depth > 0 ? ___tracy_emit_zone_begin_alloc_callstack(srcloc, depth, 1)
                    : ___tracy_emit_zone_begin_alloc(srcloc, 1);
    if (ctx.active)
        t_tracy.zones[t_tracy.zone_depth++] = ctx;
    else
        snx_tracy_leave(); /* an inactive begin freed the srcloc itself */
    return ctx;
}

void snx_tracy_zone_end_transient(TracyCZoneCtx ctx) {
    /* Same stack and count as a regular zone, so shutdown ending it first cannot lead to a second event. */
    snx_tracy_zone_end(ctx);
}

void snx_tracy_zone_end(TracyCZoneCtx ctx) {
    int i;
    if (!ctx.active) return;
    /* Shutdown ends this thread's zones itself, so the scope end that follows finds none: ending it again would emit
       a second end for it. */
    for (i = t_tracy.zone_depth - 1; i >= 0 && t_tracy.zones[i].id != ctx.id; i--) {}
    if (i < 0) return;
    if (i + 1 < t_tracy.zone_depth)
        memmove(&t_tracy.zones[i], &t_tracy.zones[i + 1],
                sizeof t_tracy.zones[0] * (size_t)(t_tracy.zone_depth - i - 1));
    t_tracy.zone_depth--;
    ___tracy_emit_zone_end(ctx);
    snx_tracy_leave();
}

/* Remembers the name for a later attach, and sends it now when a server can see it. */
void snx_tracy_thread_name(const char* name) {
    if (!name || !*name) return;
    t_tracy.hint[0] = 0; /* an unhinted name replaces a hinted one */
    snx_thread_name(name); /* the system's name too, as the build without Tracy gives it (tracy.h) */
    snprintf(t_tracy.name, sizeof t_tracy.name, "%s", name);
    SNX_TRACY_EMIT(TracyCSetThreadName(t_tracy.name));
}

/* The name with its group hint. Both are copied, since the bridge may read them after this call and tracy_announce
   reads them at each new connection. */
void snx_tracy_thread_name_hint(const char* name, const char* hint) {
    if (!name || !*name) return;
    snx_thread_name(name);
    snprintf(t_tracy.name, sizeof t_tracy.name, "%s", name);
    if (hint && *hint)
        snprintf(t_tracy.hint, sizeof t_tracy.hint, "%s", hint);
    else
        t_tracy.hint[0] = 0;
    SNX_TRACY_EMIT({
        if (t_tracy.hint[0])
            snx_tracy_conn_thread_name_hint(t_tracy.name, t_tracy.hint);
        else
            TracyCSetThreadName(t_tracy.name);
    });
}

void snx_tracy_fiber_enter(uint32_t slot, uint32_t kind, const char* name) {
    if (!name || !*name || t_tracy.fiber_depth >= SNX_TRACY_FIBER_MAX) return;
    if (slot >= SNX_FIBER_SLOTS || kind >= SNX_FIBER_KINDS) return;
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected) {
        /* No leave without an enter: a connection that came and went while this thread was not looking would
           otherwise leave a fiber the server never saw. The pair is pushed only when both events went out. */
        char* dst = g_fiber_names[slot][kind];
        snprintf(dst, SNX_TRACY_FIBER_NAME_MAX, "%s", name);
        TracyCFiberEnter(dst);
        t_tracy.fiber_slot[t_tracy.fiber_depth] = (uint8_t)slot;
        t_tracy.fiber_kind[t_tracy.fiber_depth] = (uint8_t)kind;
        t_tracy.fiber_conn[t_tracy.fiber_depth] = snx_tracy_connection_id();
        t_tracy.fiber_depth++;
    }
    snx_tracy_leave();
}

void snx_tracy_fiber_leave(void) {
    int depth;
    /* Pop the pair whatever the connection does: a disconnect here must not leave a depth that leave_all cannot
       clear. Only the events depend on a server being there. */
    if (t_tracy.fiber_depth <= 0) return;
    if (!snx_tracy_enter()) {
        t_tracy.fiber_depth--;
        return;
    }
    depth = --t_tracy.fiber_depth;
    const uint64_t conn = snx_tracy_connection_id();
    /* A fiber entered on a connection that has since gone is unknown to the current server, so ending it now would
       leave a fiber that server never saw. Only a matching connection gets the end. */
    if (TracyCIsConnected && t_tracy.fiber_conn[depth] == conn) TracyCFiberLeave;
    /* A nested leave resumes the parent that switched away: re-enter it from the parent's slot, not the one that just
       left, and only on the parent's connection. Using the leaving slot or a changed connection leaves an unmatched
       fiber on the server. */
    if (TracyCIsConnected && depth > 0 && t_tracy.fiber_conn[depth - 1] == conn) {
        char* dst = g_fiber_names[t_tracy.fiber_slot[depth - 1]][t_tracy.fiber_kind[depth - 1]];
        if (*dst) TracyCFiberEnter(dst);
    }
    snx_tracy_leave();
}

/* The plots' formats: the byte counts as sizes, the shares as percent, the idle mark as steps; a plot not listed here
   keeps Tracy's default format. This fixed table batches the runtime's own names at each connect. The dynamic cache
   below carries names the runtime cannot list here (a caller's literal), sent on their first emission instead. */
static void tracy_plots_send(void) {
    static const struct {
        const char* name;
        int type, step, fill;
        uint32_t color;
    } cfg[] = {
        {"gpu memory", 1, 0, 1, 0x2ECC71u},
        {"gpu memory budget", 1, 0, 1, 0x2ECC71u},
        {"own bytes", 1, 0, 1, 0x2ECC71u},
        {"texture uploads", 0, 0, 1, 0},
        {"texture cache hits/s", 0, 0, 1, 0},
        {"texture cache misses/s", 0, 0, 1, 0},
        {"texture upload MB/s", 0, 0, 1, 0},
        {"texture frees/s", 0, 0, 1, 0},
        {"own traps/s", 0, 0, 1, 0},
        {"own land MB/s", 0, 0, 1, 0},
        {"syncpt wait ms", 0, 0, 1, 0},
        {"present fence wait ms", 0, 0, 1, 0},
        {"present pace ms", 0, 0, 1, 0},
        {"syncpt waits/s", 0, 0, 1, 0},
        {"syncpt timeouts/s", 0, 0, 1, 0},
        {"hid updates/s", 0, 0, 1, 0},
        {"audio mix ms", 0, 0, 1, 0},
        {"audio frame ms", 0, 0, 1, 0},
        {"audio queued ms", 0, 0, 1, 0},
        {"audio underruns", 0, 1, 0, 0},
        {"guest memory", 1, 0, 1, 0x2ECC71u},
        {"compute cpu reads", 0, 0, 1, 0},
        {"gpu thread idle", 0, 1, 0, 0},
        {"frame ms", 0, 0, 1, 0xE67E22u},
        {"hitch class", 0, 1, 0, 0xE67E22u},
        {"draws", 0, 0, 1, 0},
        {"dispatches", 0, 0, 1, 0x3498DBu},
        {"shaders", 0, 0, 1, 0},
        {"host heap", 1, 0, 1, 0x2ECC71u},
        {"host rss", 1, 0, 1, 0x2ECC71u},
        {"host free", 1, 0, 1, 0x2ECC71u},
        {"guest cpu", 2, 0, 1, 0},
        {"process cpu", 2, 0, 1, 0},
        {"guest threads", 0, 0, 1, 0},
        {"svc/s", 0, 0, 1, 0},
        {"svc miss/s", 0, 0, 1, 0},
        {"nce outcalls/s", 0, 0, 1, 0},
        {"nce hooks/s", 0, 0, 1, 0},
        {"nce kicks/s", 0, 0, 1, 0},
        {"guest top thread ms", 0, 0, 1, 0},
        {"gpu gap: dispatches", 0, 0, 1, 0xE67E22u},
        {"gpu gap: copies", 0, 0, 1, 0xE67E22u},
        {"gpu gap: barriers", 0, 0, 1, 0xE67E22u},
        {"gpu gap: nothing", 0, 0, 1, 0xE67E22u},
        {"gpu idle: waited", 0, 0, 1, 0xE67E22u},
        {"gpu idle: frame", 0, 0, 1, 0xE67E22u},
        {"gpu idle: recording", 0, 0, 1, 0xE67E22u},
        {"gpu idle: queued", 0, 0, 1, 0xE67E22u},
        {"gpu busy", 2, 0, 1, 0x3498DBu},
        {"gpu submissions/frame", 0, 0, 1, 0x3498DBu},
        {"copy MB", 1, 0, 1, 0x2ECC71u},
        {"jit compile ms", 0, 0, 1, 0},
        {"jit batch size", 0, 0, 1, 0},
        {"jit batch functions", 0, 0, 1, 0},
        {"jit compiled pcs", 0, 0, 1, 0},
        {"jit cache loads", 0, 0, 1, 0},
        {"jit batches", 0, 0, 1, 0},
        {"jit queue depth", 0, 0, 1, 0},
        {"jit failures", 0, 0, 1, 0},
        {"jit dropped batches", 0, 0, 1, 0},
        {"jit miss ms", 0, 0, 1, 0},
        {"submits pending", 0, 0, 1, 0},    {"compute pending", 0, 0, 1, 0},    {"pipelines", 0, 0, 1, 0},
        {"pipeline make ms", 0, 0, 1, 0},   {"pipeline wait ms", 0, 0, 1, 0},   {"pipeline cache hits", 0, 0, 1, 0},
        {"io read MB/s", 0, 0, 1, 0},       {"io write MB/s", 0, 0, 1, 0},
        /* the renderer's per-frame counters and the resource categories, which the slow-frame log used to keep */
        {"shader translate ms", 0, 0, 1, 0},  {"texture upload count", 0, 0, 1, 0},
        {"render targets made", 0, 0, 1, 0},  {"vk finish ms", 0, 0, 1, 0},
        {"texture pipeline count", 0, 0, 1, 0}, {"texture pipeline ms", 0, 0, 1, 0},
        {"gpu wait: finish ms", 0, 0, 1, 0xE67E22u},  {"gpu wait: frame slot reuse ms", 0, 0, 1, 0xE67E22u},
        {"gpu wait: a release or result's submit ms", 0, 0, 1, 0xE67E22u},
        {"vk resource: image requirements ms", 0, 0, 1, 0}, {"vk resource: image allocation ms", 0, 0, 1, 0},
        {"vk resource: image destruction ms", 0, 0, 1, 0},  {"vk resource: target setup ms", 0, 0, 1, 0},
        {"vk resource: target reload ms", 0, 0, 1, 0},      {"vk resource: surface reclaim ms", 0, 0, 1, 0},
        {"vk resource: buffer maintenance ms", 0, 0, 1, 0}, {"vk resource: buffer cache build ms", 0, 0, 1, 0},
        /* the GPU queue's waits, holds and reasons for a submission */
        {"gpu round: finish ms", 0, 0, 1, 0xE67E22u},  {"gpu round: frame slot reuse ms", 0, 0, 1, 0xE67E22u},
        {"gpu round: a release or result's submit ms", 0, 0, 1, 0xE67E22u}, {"gpu hold: passes ms", 0, 0, 1, 0},
        {"gpu hold: dispatches ms", 0, 0, 1, 0}, {"gpu hold: copies ms", 0, 0, 1, 0},
        {"gpu hold: nothing ms", 0, 0, 1, 0},  {"gpu submit: other", 0, 0, 1, 0x3498DBu},
        {"gpu submit: present", 0, 0, 1, 0x3498DBu},   {"gpu submit: finish", 0, 0, 1, 0x3498DBu},
        {"gpu submit: 1 ms", 0, 0, 1, 0x3498DBu},      {"gpu submit: readback", 0, 0, 1, 0x3498DBu},
        {"gpu submit: frame end", 0, 0, 1, 0x3498DBu}, {"gpu submit: split", 0, 0, 1, 0x3498DBu},
        {"gpu submit: background", 0, 0, 1, 0x3498DBu},
        /* the IPC window's totals and its most costly commands, by rank */
        {"ipc req/s", 0, 0, 1, 0},         {"ipc handler ms", 0, 0, 1, 0},
        {"ipc commands", 0, 0, 1, 0},      {"ipc other req/s", 0, 0, 1, 0},
        {"ipc top 0 ms", 0, 0, 1, 0},      {"ipc top 1 ms", 0, 0, 1, 0},
        {"ipc top 2 ms", 0, 0, 1, 0},      {"ipc top 3 ms", 0, 0, 1, 0},
        {"ipc top 4 ms", 0, 0, 1, 0},      {"ipc top 5 ms", 0, 0, 1, 0},
        {"ipc top 6 ms", 0, 0, 1, 0},      {"ipc top 7 ms", 0, 0, 1, 0},
        /* the guest's busiest SVCs and hook families, and the NCE's own counters */
        {"top svc 0/s", 0, 0, 1, 0},       {"top svc 1/s", 0, 0, 1, 0},
        {"top svc 2/s", 0, 0, 1, 0},       {"top svc 3/s", 0, 0, 1, 0},
        {"top svc 4/s", 0, 0, 1, 0},       {"top svc 5/s", 0, 0, 1, 0},
        {"top hook 0/s", 0, 0, 1, 0},      {"top hook 1/s", 0, 0, 1, 0},
        {"top hook 2/s", 0, 0, 1, 0},      {"top hook 3/s", 0, 0, 1, 0},
        {"top hook 4/s", 0, 0, 1, 0},      {"nce svc/s", 0, 0, 1, 0},
        {"nce trap/s", 0, 0, 1, 0},        {"nce ticks/s", 0, 0, 1, 0},
        {"nce run/s", 0, 0, 1, 0},
        /* the JIT's compiled code and its AOT lookups */
        {"jit code KiB", 0, 0, 1, 0},      {"jit instructions", 0, 0, 1, 0},
        {"jit variant hits", 0, 0, 1, 0},  {"jit variant misses", 0, 0, 1, 0},
    };
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected)
        for (size_t i = 0; i < sizeof cfg / sizeof *cfg; i++)
            ___tracy_emit_plot_config(cfg[i].name, cfg[i].type, cfg[i].step, cfg[i].fill, cfg[i].color);
    snx_tracy_leave();
}

/* These stay as file-scope globals, not in the file's state struct: t_tracy is thread-local, while the plot cache and
   the GPU arrays below are shared by every thread. Moving either into t_tracy would give each thread a copy and lose
   the once-per-name, once-per-context behavior.

   The dynamic plots' formats are cached per name and connection. Plots are emitted from several threads, so a mutex
   guards the cache; a new connection's server was sent none of the formats the last one's was, so its arrival
   empties the cache and the formats go out again. */
#define g_plot_cfg_lock (g_tracy.plot_cfg_lock)
#define g_plot_cfg      (g_tracy.plot_cfg)
#define g_plot_cfg_n    (g_tracy.plot_cfg_n)

/* A plot the caller formats itself: the format is a one-time hint per name, keyed by the name's address (the
   caller's literal), so a plot emitted every frame sends its config only once. The cache also serves the one
   runtime name that is not a literal in the fixed table. The mutex is taken on each call, including a hit: this
   is a profiling build and the cost has not been measured. Tracy's plot formats are already
   0 number, 1 memory, 2 percentage, 3 watt; the table maps and clamps the caller's value to those. */
void snx_tracy_plot_cfg(const char* name, int type, int step, int fill, uint32_t color) {
    static const int32_t format[] = {TracyPlotFormatNumber, TracyPlotFormatMemory, TracyPlotFormatPercentage,
                                     TracyPlotFormatWatt};
    uint64_t conn;
    int i;
    if (!name) return;
    if (!snx_tracy_enter()) return;
    pm_lock(&g_plot_cfg_lock);
    if (!TracyCIsConnected) {
        pm_unlock(&g_plot_cfg_lock);
        snx_tracy_leave();
        return;
    }
    conn = snx_tracy_connection_id();
    if (g_plot_cfg_n && g_plot_cfg[0].conn != conn) g_plot_cfg_n = 0;
    for (i = 0; i < g_plot_cfg_n; i++)
        if (g_plot_cfg[i].name == name) break;
    if (i == g_plot_cfg_n && g_plot_cfg_n < SNX_TRACY_PLOT_CFG_MAX) {
        int t = type < 0 ? 0 : type > 3 ? 3 : type;
        ___tracy_emit_plot_config(name, format[t], step, fill, color);
        g_plot_cfg[g_plot_cfg_n].name = name;
        g_plot_cfg[g_plot_cfg_n].conn = conn;
        g_plot_cfg_n++;
    }
    pm_unlock(&g_plot_cfg_lock);
    snx_tracy_leave();
}

/* The server keeps unnamed marks from every thread in one frame set, keyed on the name alone, and seeds that set at
   each connection. A thread's first mark on a connection gives a span from that seed, so each presenting thread
   skips its first mark on each connection. vi and NVN present on different guest threads. */

/* One unnamed frame mark. final bypasses the first-mark skip: the shutdown mark closes the last continuous frame and
   must go out even when no frame was seen on this connection. */
static void tracy_frame_mark(int final) {
    uint64_t conn;
    if (!snx_tracy_gate_open() || !snx_tracy_enter()) return;
    /* Both the attachment check and the connection id read Tracy's data, so hold the emission count across them. */
    if (!snx_tracy_attached()) {
        snx_tracy_leave();
        return;
    }
    conn = snx_tracy_connection_id();
    if (!final && conn != t_tracy.frame_conn) {
        t_tracy.frame_conn = conn;
        snx_tracy_leave();
        return; /* the first mark from this thread on this connection is only a seed */
    }
    t_tracy.frame_conn = conn;
    if (!TracyCIsConnected) {
        snx_tracy_leave();
        return;
    }
    ___tracy_emit_frame_mark(0);
    snx_tracy_leave();
}

void snx_tracy_frame_mark(void) { tracy_frame_mark(0); }

/* A named set has its own seed: the first mark for a name on a connection gives the server the set's baseline, and
   the marks after it are frames. The name is interned in the table above, because Tracy queues its address for the
   server to read later. A table full of names drops later ones instead of evicting an address the server may still
   need. */
void snx_tracy_frame_mark_named(const char* name) {
    const char* stored;
    uint64_t conn;
    int i, seed;
    if (!name || !*name || !snx_tracy_gate_open() || !snx_tracy_enter()) return;
    if (!snx_tracy_attached()) {
        snx_tracy_leave();
        return;
    }
    conn = snx_tracy_connection_id();
    pm_lock(&g_tracy_frame_names.lock);
    for (i = 0; i < g_tracy_frame_names.n; i++)
        if (!strcmp(g_tracy_frame_names.slot[i].name, name)) break;
    if (i == g_tracy_frame_names.n) {
        if (g_tracy_frame_names.n == SNX_TRACY_FRAME_NAME_MAX) {
            pm_unlock(&g_tracy_frame_names.lock);
            snx_tracy_leave();
            return;
        }
        snprintf(g_tracy_frame_names.slot[i].name, SNX_TRACY_FRAME_NAME_LEN, "%s", name);
        g_tracy_frame_names.slot[i].conn = 0;
        g_tracy_frame_names.n++;
    }
    stored = g_tracy_frame_names.slot[i].name;
    seed = g_tracy_frame_names.slot[i].conn != conn;
    g_tracy_frame_names.slot[i].conn = conn;
    pm_unlock(&g_tracy_frame_names.lock);
    if (!seed && TracyCIsConnected) ___tracy_emit_frame_mark(stored);
    snx_tracy_leave();
}

int snx_tracy_frame_start(const char* name) {
    int sent = 0;
    if (!name || !*name || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected) {
        ___tracy_emit_frame_mark_start(name);
        sent = 1;
    }
    snx_tracy_leave();
    return sent;
}

void snx_tracy_frame_at_scope(SnxTracyFrame* f) {
    if (!f->started) return;
    f->started = 0;
    SNX_TRACY_EMIT(___tracy_emit_frame_mark_end(f->name));
}

/* The section's enter and leave are single events, not a zone's pair on the stack. The server sends no id when it
   is not connected (and for an empty name); 0 also means the leave has nothing to end. A name is a format for Tracy,
   so it is passed as an argument, never as the format itself. */
SnxTracySection snx_tracy_section_start_c(const char* name, uint16_t category) {
    SnxTracySection s = {0};
    if (!name || !*name || !snx_tracy_enter()) return s;
    if (TracyCIsConnected) {
        s.id = ___tracy_section_enter(category, "%s", name);
        s.started = s.id != 0;
    }
    snx_tracy_leave();
    return s;
}

SnxTracySection snx_tracy_section_start(const char* name) { return snx_tracy_section_start_c(name, 0); }

/* One name per category, announced once as the client starts. Tracy defers it to each server that connects, so this
   does not wait for one; it only needs the client's data to exist, which the startup call has just made. */
void snx_tracy_section_setup(uint16_t category, const char* name) {
    if (!name || !*name || !snx_tracy_enter()) return;
    ___tracy_section_setup(category, "%s", name);
    snx_tracy_leave();
}

void snx_tracy_section_at_scope(SnxTracySection* s) {
    if (!s || !s->started) return;
    s->started = 0;
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected) TracyCSectionLeave(s->id);
    snx_tracy_leave();
}

/* The GPU track's emissions, each gated on its own: the caller calls them as they come. A context is announced
   once for the client's lifetime — the deferred queue replays it at every connection, and a second live
   announcement over the replayed one would corrupt the server's context. A zone's end and time carry the
   connection the begin went out on, and are dropped on another one, where the server would fault on a query it
   never saw. */
#define SNX_TRACY_GPU_CTX 8
#define g_gpu_sent  (g_tracy.gpu_sent)
#define g_gpu_query (g_tracy.gpu_query)

static int gpu_ctx_ok(int ctx) { return ctx >= 0 && ctx < SNX_TRACY_GPU_CTX; }

/* The server raises its global time watermark on every raw GPU timestamp it sees. A device counter is not a CPU
   time, so an uncalibrated device can leave the trace's last frame ending years after the run. Anchor each context's
   device ticks to the CPU clock at the first pair and send the ticks on: the mapper stays in device ticks, turning
   the CPU nanoseconds into ticks with the context's period (ns per tick), and the server scales by that same period.
   The GPU track keeps its relative times, and no event can push the capture's end past the app's own clock. */
#define g_gpu_offset   (g_tracy.gpu_offset)
#define g_gpu_period   (g_tracy.gpu_period) /* the context's ns per device tick, from its announcement */
#define g_gpu_anchored (g_tracy.gpu_anchored)
#define g_tracy_start_ns (g_tracy.start_ns) /* the client's clock origin */

static int64_t tracy_gpu_map(int ctx, int64_t gpu_time) {
    /* A period below one ns per tick, or not a number, would make the clock division overflow or cast badly, so keep
       a floor. Unsigned subtraction wraps by definition: a device clock far from the CPU's would overflow a signed
       one. The server puts GPU zones on the CPU timeline from the point the client started, so the CPU side of the
       anchor must be the same: time since start, not time since boot. Using plat_ns() here put every GPU zone hours
       past the capture and made the trace's last frame span those hours. */
    float period = g_gpu_period[ctx];
    uint64_t cpu, now = plat_ns();
    int64_t mapped;
    if (!(period >= 1.0f)) period = 1.0f; /* also covers an unset, negative or NaN period */
    cpu = (uint64_t)((double)(now - (now >= g_tracy_start_ns ? g_tracy_start_ns : 0)) / (double)period);
    if (!g_gpu_anchored[ctx]) {
        g_gpu_offset[ctx] = (int64_t)((uint64_t)gpu_time - cpu);
        g_gpu_anchored[ctx] = 1;
    }
    mapped = (int64_t)((uint64_t)gpu_time - (uint64_t)g_gpu_offset[ctx]);
    /* A query whose result was not ready, or a slot whose pending write was overwritten, can carry a counter that
       is far outside this run. The server raises its time watermark on every raw GPU stamp, so one such value makes
       the capture's last frame end minutes after the run. Keep every mapped stamp within a generous window of the
       CPU's own time; a healthy device stays far inside it. */
    {
        uint64_t window = (uint64_t)(10.0 * 1000000000.0 / (double)period); /* ten seconds in device ticks */
        uint64_t hi = cpu + window, lo = cpu > window ? cpu - window : 0;
        if ((uint64_t)mapped > hi) mapped = (int64_t)hi;
        if (mapped < (int64_t)lo) mapped = (int64_t)lo;
    }
    return mapped;
}

int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated, const char* name) {
    int announced = 0;
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return 0;
    if (!(period >= 1.0f)) period = 1.0f; /* a zero, negative or NaN period would poison the server's GPU track */
    if (TracyCIsConnected && !g_gpu_sent[ctx]) {
        char fallback[32];
        const char* ctx_name = name;
        struct ___tracy_gpu_new_context_data c;
        g_gpu_period[ctx] = period;
        c.gpuTime = tracy_gpu_map(ctx, gpu_time);
        c.period = period;
        c.context = (uint8_t)ctx;
        c.flags = calibrated ? 1 : 0;
        c.type = 2;
        ___tracy_emit_gpu_new_context_serial(c);
        /* Name the context in the GUI even when the caller has none: its own name, or a per-context fallback. The
           name goes out with the context (once for the client's life), and the client's deferred queue replays both
           to each new connection. The client copies the bytes, so a stack fallback is safe here. */
        if (!ctx_name || !*ctx_name) {
            snprintf(fallback, sizeof fallback, "gpu context %d", ctx);
            ctx_name = fallback;
        }
        {
            struct ___tracy_gpu_context_name_data n = {(uint8_t)ctx, ctx_name, (uint16_t)strlen(ctx_name)};
            ___tracy_emit_gpu_context_name_serial(n);
        }
        g_gpu_sent[ctx] = 1;
        announced = 1;
    }
    snx_tracy_leave();
    return announced;
}

uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx, uint64_t* ep) {
    uint16_t q = 0;
    if (ep) *ep = 0;
    if (!gpu_ctx_ok(ctx) || !name || !*name || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        /* the client copies the name at the allocation, so one srcloc a zone is all it needs */
        uint64_t srcloc = ___tracy_alloc_srcloc_name(0, __FILE__, sizeof __FILE__ - 1, __func__, sizeof __func__ - 1, name,
                                                strlen(name), color);
        if (srcloc) {
            uint16_t id = (uint16_t)(g_gpu_query[ctx] + 1);
            int gpu_cs = atomic_load_explicit(&snx_tracy_gpu_zone_callstack, memory_order_relaxed);
            if (!id) id = 1; /* 0 is the caller's "none" */
            g_gpu_query[ctx] = id;
            q = id;
            if (ep) *ep = snx_tracy_connection_id();
            if (gpu_cs > 0) {
                int depth = gpu_cs > 16 ? 16 : gpu_cs;
                struct ___tracy_gpu_zone_begin_callstack_data d = {srcloc, depth, id, (uint8_t)ctx};
                ___tracy_emit_gpu_zone_begin_alloc_callstack_serial(d);
            } else {
                struct ___tracy_gpu_zone_begin_data d = {srcloc, id, (uint8_t)ctx};
                ___tracy_emit_gpu_zone_begin_alloc_serial(d);
            }
        }
    }
    snx_tracy_leave();
    return q;
}

uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx, uint64_t ep) {
    uint16_t q = 0;
    /* the gate first: the connection's id reads the client's data, which a run that never started it has none of */
    if (!query || !gpu_ctx_ok(ctx) || !snx_tracy_enter()) return 0;
    /* a pair from another connection is dropped, and leaves as it enters: the count is the shutdown's wait */
    if (ep != snx_tracy_connection_id()) {
        snx_tracy_leave();
        return q;
    }
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        uint16_t id = (uint16_t)(g_gpu_query[ctx] + 1);
        if (!id) id = 1;
        g_gpu_query[ctx] = id;
        struct ___tracy_gpu_zone_end_data d = {id, (uint8_t)ctx};
        q = id;
        ___tracy_emit_gpu_zone_end_serial(d);
    }
    snx_tracy_leave();
    return q;
}

void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx, uint64_t ep) {
    if (!query || !gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (ep != snx_tracy_connection_id()) {
        snx_tracy_leave();
        return;
    }
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        struct ___tracy_gpu_time_data t = {tracy_gpu_map(ctx, gpu_time), query, (uint8_t)ctx};
        ___tracy_emit_gpu_time_serial(t);
    }
    snx_tracy_leave();
}

void snx_tracy_gpu_sync(int64_t gpu_time, int ctx) {
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        struct ___tracy_gpu_time_sync_data s = {tracy_gpu_map(ctx, gpu_time), (uint8_t)ctx};
        ___tracy_emit_gpu_time_sync_serial(s);
    }
    snx_tracy_leave();
}

void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx) {
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        struct ___tracy_gpu_calibration_data c = {tracy_gpu_map(ctx, gpu_time), cpu_delta, (uint8_t)ctx};
        ___tracy_emit_gpu_calibration_serial(c);
    }
    snx_tracy_leave();
}

/* A frame's picture: the client's own copy is queued for the server, which puts it on the timeline at the frame's
   time. */
void snx_tracy_frame_image(const void* image, uint16_t width, uint16_t height) {
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected) TracyCFrameImage(image, width, height, 0, 0);
    snx_tracy_leave();
}

/* The named locks' events: each PMutex that pm_lock_announce named reports its waits, takes and releases here, and
   the pane shows who held it and who waited how long. The announce goes out even before a server attaches (the
   client defers it); the events are single items and drop when nobody is connected. */
SnxTracyLock* snx_tracy_lock_make(const char* name) {
    static const struct ___tracy_source_location_data srcloc = {NULL, "pm_lock", __FILE__, __LINE__, 0};
    SnxTracyLock* l;
    if (!name || !*name) return NULL;
    /* The announce and the name are deferred to a server that attaches later, so they must go out before one is
       connected: SNX_TRACY_EMIT's connected check would drop this lock from that capture. */
    if (!snx_tracy_enter()) return NULL;
    l = ___tracy_announce_lockable_ctx(&srcloc);
    if (l) ___tracy_custom_name_lockable_ctx(l, name, strlen(name));
    snx_tracy_leave();
    return l;
}

void snx_tracy_lock_free(SnxTracyLock* l) {
    if (!l || !snx_tracy_enter()) return;
    ___tracy_terminate_lockable_ctx(l);
    snx_tracy_leave();
}

void snx_tracy_lock_wait(SnxTracyLock* l) {
    if (!l) return;
    SNX_TRACY_EMIT(___tracy_before_lock_lockable_ctx(l));
}

void snx_tracy_lock_lock(SnxTracyLock* l) {
    if (!l) return;
    SNX_TRACY_EMIT(___tracy_after_lock_lockable_ctx(l));
}

void snx_tracy_lock_unlock(SnxTracyLock* l) {
    if (!l) return;
    SNX_TRACY_EMIT(___tracy_before_unlock_lockable_ctx(l));
}

void snx_tracy_lock_try(SnxTracyLock* l, int acquired) {
    if (!l || !atomic_load_explicit(&snx_tracy_lock_marks, memory_order_relaxed)) return;
    /* A failed try is an event too: the pane shows both outcomes, with the second argument saying which one. */
    SNX_TRACY_EMIT(___tracy_after_try_lock_lockable_ctx(l, acquired ? 1 : 0));
}

/* The server keeps a mark's srcloc pointer and reads it when a GUI asks, so each call site's location must stay at
   one address. A bounded table interns the file/line pairs the macros pass; the mutex keeps two threads from claiming
   one slot. With the marks setting off this code is never reached, so an unmarked run pays no table or lock cost. */
#define g_tracy_mark (g_tracy.lock_marks)

void snx_tracy_lock_mark(SnxTracyLock* l, const char* file, uint32_t line) {
    int i;
    if (!l || !atomic_load_explicit(&snx_tracy_lock_marks, memory_order_relaxed)) return;
    SNX_TRACY_EMIT({
        pm_lock(&g_tracy_mark.lock);
        for (i = 0; i < g_tracy_mark.n; i++)
            if (g_tracy_mark.slot[i].file == file && g_tracy_mark.slot[i].line == line) break;
        if (i == g_tracy_mark.n && g_tracy_mark.n < SNX_TRACY_LOCK_MARK_MAX) {
            struct ___tracy_source_location_data srcloc = {NULL, "lock mark", file, line, 0};
            g_tracy_mark.slot[i].file = file;
            g_tracy_mark.slot[i].line = line;
            g_tracy_mark.slot[i].srcloc = srcloc;
            g_tracy_mark.n++;
        }
        if (i < g_tracy_mark.n) ___tracy_mark_lockable_ctx(l, &g_tracy_mark.slot[i].srcloc);
        pm_unlock(&g_tracy_mark.lock);
    });
}

int snx_tracy_init(const char* title, const SnxTracyHost* host) {
    if (host) {
        snprintf(g_tracy.dir, sizeof g_tracy.dir, "%s", host->dir ? host->dir : "");
        snprintf(g_tracy.name, sizeof g_tracy.name, "%s", host->name && host->name[0] ? host->name : "game");
        g_tracy.capture_asked = host->capture, g_tracy.client_asked = host->client, g_tracy.headless = host->headless;
        g_tracy.log = host->log;
    }
    atomic_store_explicit(&snx_tracy_frame_images_live, g_settings.tracy_frame_images, memory_order_relaxed);
    atomic_store_explicit(&snx_tracy_memory_detail, g_settings.tracy_memory_detail != 0, memory_order_relaxed);
    if (g_settings.tracy_memory_detail)
        atomic_store_explicit(&snx_tracy_memory_detail_ever, 1, memory_order_relaxed);
    atomic_store_explicit(&snx_tracy_zone_callstack, g_settings.tracy_zone_callstack, memory_order_relaxed);
    atomic_store_explicit(&snx_tracy_memory_callstack, g_settings.tracy_memory_callstack, memory_order_relaxed);
    atomic_store_explicit(&snx_tracy_gpu_zone_callstack, g_settings.tracy_gpu_zone_callstack, memory_order_relaxed);
    atomic_store_explicit(&snx_tracy_lock_marks, g_settings.tracy_lock_marks != 0, memory_order_relaxed);
    g_tracy_start_ns = plat_ns(); /* Tracy's timeline starts at the client, so the GPU anchor uses this base */
    pc_init(&g_tracy_wait_cond); /* F10 may start the client after a run that asked for nothing here */
    pm_init(&g_tracy_capture_lock);
    pm_init(&g_tracy_wait_lock);
    pm_init(&g_tracy.plot_cfg_lock);
    pm_init(&g_tracy.frame_names.lock);
    pm_init(&g_tracy.lock_marks.lock);
    snprintf(g_tracy_title, sizeof g_tracy_title, "%s", title && title[0] ? title : "ScarletNX");
    if (!g_tracy.capture_asked && !g_tracy.client_asked) return 0;
    /* The client before the capture: the capture connects once, so it has to find the client listening. The client
       reads its port as it starts, so it is set before that. */
    tracy_port_setup(g_tracy.capture_asked);
    tracy_start_client(); /* it registers the locks, so F10's client reports them too */
    if (g_tracy.capture_asked && !tracy_capture_wait()) return 1;
    return 0;
}

int snx_tracy_console(const char* folder, const char* name, int headless) {
    char dir[SNX_PATH_SIZE];
    snprintf(dir, sizeof dir, "%s/", folder && folder[0] ? folder : ".");
    return snx_tracy_init(
        name, &(SnxTracyHost){dir, name, g_settings.tracy_capture, g_settings.tracy_profiler, headless, NULL});
}

void snx_tracy_log(int lvl, const char* text) {
    int severity;
    size_t n;
    if (!text || !snx_tracy_gate_open()) return; /* the gate first: no client means no message to measure */
    n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    if (!n) return;
    /* the log's 0 warning, 1 line, 2 verbose is the message's 3 warning, 2 info, 1 debug; SNX_LOG maps it through
       snx_tracy_severity, the one severity lookup for every message */
    severity = 3 - (lvl < 0 ? 0 : lvl > 2 ? 2 : lvl);
    SNX_LOG(text, n, severity, 0, 0);
}

void snx_tracy_fiber_leave_all(void) {
    while (t_tracy.fiber_depth > 0) snx_tracy_fiber_leave();
}

void snx_tracy_fatal(const char* text) {
    /* No pending count here: the crash path can reach this with a lock held, and leave() would take the wait lock
       when the count reached zero. A run ending here cannot have work left in flight to wait for. */
    if (!text || !snx_tracy_gate_open()) return;
    if (TracyCIsConnected) {
        size_t n = strlen(text);
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
        if (n) ___tracy_emit_logString(snx_tracy_severity(5), 0, 0, n, text);
    }
}

void snx_tracy_shutdown(void) {
    uint64_t until;
    int on;
    snx_tracy_fiber_leave_all(); /* this thread's fibers, while the client can still hear their end */
    /* Close the last continuous frame at the shutdown mark, while the gate is open and the client can still send;
       without it the capture's final frame ends at the leaked watermark. The helper rechecks the gate and the
       connection and, being final, emits even if no frame was seen on this connection. */
    tracy_frame_mark(1);
    /* Close the gate, once and only on a running client; the exchange is sequenced with the counts, so a thread that
       passed it is already counted. */
    for (;;) {
        int state = atomic_load_explicit(&g_tracy_state, memory_order_acquire);
        if (state == SNX_TRACY_STARTING) {
            tracy_wait_ns(1000000);
            continue;
        }
        on = SNX_TRACY_ON;
        if (atomic_compare_exchange_strong_explicit(&g_tracy_state, &on, SNX_TRACY_STOPPED, memory_order_seq_cst,
                                                     memory_order_acquire))
            break;
        /* Another thread is writing the capture. Wait only when it won; a shutdown before the start, or a second
           one after the client is done, has nothing to wait for. */
        while (atomic_load_explicit(&g_tracy_state, memory_order_acquire) == SNX_TRACY_STOPPED)
            tracy_wait_ns(1000000);
        return;
    }
    tracy_sampling_stop();
    /* End this thread's nested zones, each releasing its own count. */
    while (t_tracy.zone_depth > 0) snx_tracy_zone_end(t_tracy.zones[t_tracy.zone_depth - 1]);
    /* Wait for other threads; snx_tracy_leave wakes this whenever the count reaches zero. If one stays in a
       zone, leave the client for _exit to close. */
    until = plat_ns() + 2000000000ull;
    for (;;) {
        int64_t left;
        if (!atomic_load_explicit(&g_tracy_pending, memory_order_acquire)) {
            ___tracy_shutdown_profiler();
            tracy_capture_finish(1);
            atomic_store_explicit(&g_tracy_state, SNX_TRACY_DONE, memory_order_release);
            return;
        }
        pm_lock(&g_tracy_wait_lock);
        /* the bound is computed under the lock, right before the wait: a wait cannot be given a stale one and
           run past the deadline */
        left = (int64_t)(until - plat_ns());
        if (left > 0 && atomic_load_explicit(&g_tracy_pending, memory_order_acquire))
            pc_wait(&g_tracy_wait_cond, &g_tracy_wait_lock, left);
        pm_unlock(&g_tracy_wait_lock);
        if (left <= 0) break;
    }
    /* The socket closes at _exit; leave tracy-capture to write the trace. */
    tracy_capture_finish(0);
    atomic_store_explicit(&g_tracy_state, SNX_TRACY_DONE, memory_order_release);
}
#else
/* Without a client no parameter callback can change this; it stays at zero for readers of the stub in tracy.h. */
_Atomic int snx_tracy_frame_images_live;
#endif
