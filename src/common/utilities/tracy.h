/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Tracy zones, plots and memory events, for every console's runtime; compiled out without SNX_TRACY. */
#ifndef SNX_TRACY_H
#define SNX_TRACY_H

#ifndef __cplusplus
#include <stdatomic.h>
#endif
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The calibration anchor a caller keeps for one GPU context, with the connection it was announced on. The context
   is announced once, and a new connection's server re-anchored at the replayed context's original pair, so the
   client's CPU delta is measured from the announcement again, not from the last calibration event (which is what
   a zeroing reset would do). A context announced without a clock pair never gets a delta: the server asserts on
   calibration for a context it was told is uncalibrated. */
typedef struct {
    int calibrated;     /* the context was announced calibrated */
    int64_t cpu_anchor; /* the client's clock at the announcement, 0 without one */
    int64_t cpu_prev;   /* that anchor, or the last calibration's clock */
    int64_t gpu_anchor; /* the device clock at the announcement */
    int64_t gpu_prev;   /* that anchor, or the last calibration's device clock */
    uint64_t conn;      /* the connection this anchor belongs to: a new one re-anchors it */
} SnxTracyGpuCal;

/* What the runtime that starts the client gives it: the folder its tools are looked for beside and its captures go
   in (captures/), with its separator; the name a capture's file takes; whether the run asked for a capture of its own
   (--tracy-capture) or the client alone (--tracy); whether it has no screen to open a trace on; and its log, a line
   with its end, warn 1 for a failure (NULL: stderr) */
typedef struct {
    const char* dir;
    const char* name;
    int capture, client, headless;
    void (*log)(int warn, const char* line);
} SnxTracyHost;

/* A high-level phase, on Tracy's own section track. Unlike a zone a section is not a stack: the pair of events
   brackets one piece of work, and the id from the enter is what the scope's end sends. */
typedef struct {
    int started;
    uint32_t id;
} SnxTracySection;

/* A guest's wait gets a fiber of its own. The name's address is the fiber's identity for the server, so tracy.c
   keeps one name of each slot and kind for the capture's life; a caller's stack buffer would not do. */
#define SNX_FIBER_SLOTS 256
#define SNX_FIBER_KINDS 8
#define SNX_FIBER_ENTER(slot, kind, name) snx_tracy_fiber_enter((uint32_t)(slot), (uint32_t)(kind), (name))
#define SNX_FIBER_LEAVE() snx_tracy_fiber_leave()
#define SNX_FIBER_LEAVE_ALL() snx_tracy_fiber_leave_all()

#if defined(SNX_TRACY)
#include <tracy/TracyC.h>

/* The C++ client's bridges, in tracy_conn.cpp. The C layer owns the client's lifetime and calls these as it starts;
   the C++ types stay on that side of the header. */
void snx_tracy_conn_set_program_name(const char* name);
void snx_tracy_conn_register_parameters(void);
void snx_tracy_conn_register_source_callback(void);
void snx_tracy_source_add(const char* name, const char* bytes, size_t size);
void snx_tracy_conn_thread_name_hint(const char* name, const char* hint);

/* Every emission passes this gate; shutdown closes it. */
int snx_tracy_enter(void);
void snx_tracy_leave(void);
TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc);
TracyCZoneCtx snx_tracy_zone_begin_cs(const struct ___tracy_source_location_data* srcloc, int depth);
void snx_tracy_zone_end(TracyCZoneCtx ctx);
static inline void snx_tracy_zone_end_at_scope(TracyCZoneCtx* ctx) { snx_tracy_zone_end(*ctx); }
/* A transient zone's name need not outlive the call: the begin copies it into an srcloc the client owns, so a
   formatted or stack name is safe. The end shares the regular zone stack (and its shutdown/double-end handling). */
TracyCZoneCtx snx_tracy_zone_begin_transient(const char* name, int callstack, int depth);
void snx_tracy_zone_end_transient(TracyCZoneCtx ctx);
static inline void snx_tracy_zone_end_transient_at_scope(TracyCZoneCtx* ctx) { snx_tracy_zone_end_transient(*ctx); }

/* The zone ends at the end of the block it opens in, a return included; one that ends earlier calls SNX_ZONE_END.
   All three public forms share this one: the color goes in the source location, the callstack selector picks the
   begin. Both selectors are constants at the call site, so one of the two begins folds away. */
#define SNX_ZONE_IMPL(ctx, name, color, cs, depth)                                                                     \
    static const struct ___tracy_source_location_data TracyConcat(snx_tracy_source_, TracyLine) =                     \
        {name, __func__, TracyFile, (uint32_t)TracyLine, (uint32_t)(color)};                                           \
    [[gnu::cleanup(snx_tracy_zone_end_at_scope)]] TracyCZoneCtx ctx =                                                  \
        (cs) ? snx_tracy_zone_begin_cs(&TracyConcat(snx_tracy_source_, TracyLine), (depth))                            \
             : snx_tracy_zone_begin(&TracyConcat(snx_tracy_source_, TracyLine))
#define SNX_ZONE(ctx, name) SNX_ZONE_IMPL(ctx, name, 0, 0, 0)
/* a zone in a named color, for the timeline's kinds at a glance */
#define SNX_ZONE_C(ctx, name, color) SNX_ZONE_IMPL(ctx, name, (color), 0, 0)
/* a zone that also captures a callstack, for the flame graphs. A depth of 0 uses snx_tracy_zone_callstack. */
#define SNX_ZONE_CS(ctx, name, depth) SNX_ZONE_IMPL(ctx, name, 0, 1, (depth))
/* Transient zones take an expression for the name (evaluated once, copied by the client) rather than a literal. The
   two forms mirror SNX_ZONE and SNX_ZONE_CS; a depth of 0 with CS takes the zone-callstack setting, as it does there. */
#define SNX_ZONE_TRANSIENT_IMPL(ctx, name, cs, depth)                                                                   \
    [[gnu::cleanup(snx_tracy_zone_end_transient_at_scope)]] TracyCZoneCtx ctx =                                        \
        snx_tracy_zone_begin_transient((name), (cs), (depth))
#define SNX_ZONE_TRANSIENT(ctx, name) SNX_ZONE_TRANSIENT_IMPL(ctx, name, 0, 0)
#define SNX_ZONE_TRANSIENT_CS(ctx, name, depth) SNX_ZONE_TRANSIENT_IMPL(ctx, name, 1, (depth))
/* a short emission: gated, and skipped when nothing is attached */
#define SNX_TRACY_EMIT(...)                                                                                            \
    do {                                                                                                               \
        if (snx_tracy_enter()) {                                                                                       \
            if (TracyCIsConnected) __VA_ARGS__;                                                                        \
            snx_tracy_leave();                                                                                         \
        }                                                                                                              \
    } while (0)
#define SNX_ZONE_TEXT(ctx, ...) SNX_TRACY_EMIT(TracyCZoneTextF(ctx, __VA_ARGS__))
/* Rename an open zone on the timeline: a counted string, or a formatted one. The fmt is Tracy's, so it must be a
   string literal with arguments that match it. */
#define SNX_ZONE_NAME(zctx, text, size) SNX_TRACY_EMIT(TracyCZoneName(zctx, (text), (size_t)(size)))
#define SNX_ZONE_NAME_F(zctx, fmt, ...) SNX_TRACY_EMIT(___tracy_emit_zone_name_fmt((zctx), fmt, __VA_ARGS__))
#define SNX_ZONE_VALUE(ctx, value) SNX_TRACY_EMIT(TracyCZoneValue(ctx, (uint64_t)(value)))
/* a one-line mark on the timeline, where a plot would say how often and a message says which one. Tracy keeps the
   name pointer until the scope ends, so it must have static lifetime or outlive the scope. */
#define SNX_MSG(text, size) SNX_TRACY_EMIT(TracyCMessage(text, size))
/* a message with its own severity, color and callstack depth. Severity 0 is trace, 1 debug, 2 info, 3 warning,
   4 error, 5 fatal; out-of-range values are clamped. One lookup for every message, shared with the test stub. */
static inline int8_t snx_tracy_severity(int severity) {
    static const int8_t sev[] = {TracyMessageSeverityTrace, TracyMessageSeverityDebug, TracyMessageSeverityInfo,
                                 TracyMessageSeverityWarning, TracyMessageSeverityError, TracyMessageSeverityFatal};
    if (severity < 0) severity = 0;
    if (severity > 5) severity = 5;
    return sev[severity];
}
#define SNX_LOG(text, size, severity, color, depth)                                                                   \
    SNX_TRACY_EMIT(___tracy_emit_logString(snx_tracy_severity(severity), (int32_t)(color), (int32_t)(depth),          \
                                           (size_t)(size), (const char*)(text)))
/* An info-severity message with an explicit callstack depth. It goes through SNX_LOG, so the severity lookup and the
   emission gate are the same as for any other message. */
#define SNX_MSG_CS(text, size, depth) SNX_LOG(text, size, 2, 0, depth)
/* a zone whose text or value is known as it opens */
#define SNX_ZONE_T(ctx, name, ...) SNX_ZONE(ctx, name); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_V(ctx, name, value) SNX_ZONE(ctx, name); SNX_ZONE_VALUE(ctx, value)
#define SNX_ZONE_TV(ctx, name, value, ...) SNX_ZONE_V(ctx, name, value); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_END(ctx) snx_tracy_zone_end(ctx)
/* Recolors a zone that is already open; 0xRRGGBB in the caller's number. */
#define SNX_ZONE_COLOR(zctx, color) SNX_TRACY_EMIT(___tracy_emit_zone_color(zctx, (uint32_t)(color)))
#define SNX_MEM_ALLOC(ptr, size, pool) SNX_TRACY_EMIT(TracyCAllocN((const void*)(ptr), (size_t)(size), (pool)))
#define SNX_MEM_FREE(ptr, pool) SNX_TRACY_EMIT(TracyCFreeN((const void*)(ptr), (pool)))
/* The detailed pools cost an event on every game malloc and free. tracy_memory_detail=1 turns them on. These gates
   start at the settings' values, and a slow frame can raise them for a few frames; the allocator and the zone begins
   read them on other threads without a lock, so each is atomic. At a depth of 0 Tracy's alloc and free take the same
   path as before. */
/* The seconds between frame images, read by the renderer and written by the C++ parameter callback while a server
   is attached. Others read it without a lock, so it is atomic; the value changes only while the client is up. */
extern _Atomic int snx_tracy_frame_images_live;
extern _Atomic int snx_tracy_memory_detail;
/* Once detail is on, it stays on for the run: a GUI that turns it off must not leave earlier allocations tracked
   without their frees, which would make Tracy report duplicate live addresses when the allocator reuses one. */
extern _Atomic int snx_tracy_memory_detail_ever;
extern _Atomic int snx_tracy_memory_callstack;
extern _Atomic int snx_tracy_zone_callstack;
#define SNX_MEM_ALLOC_D(ptr, size, pool)                                                                               \
    do {                                                                                                               \
        if (atomic_load_explicit(&snx_tracy_memory_detail_ever, memory_order_relaxed))                                     \
            SNX_TRACY_EMIT(TracyCAllocNS((const void*)(ptr), (size_t)(size),                                          \
                                         atomic_load_explicit(&snx_tracy_memory_callstack, memory_order_relaxed),      \
                                         (pool)));                                                                     \
    } while (0)
#define SNX_MEM_FREE_D(ptr, pool)                                                                                      \
    do {                                                                                                               \
        if (atomic_load_explicit(&snx_tracy_memory_detail_ever, memory_order_relaxed))                                     \
            SNX_TRACY_EMIT(TracyCFreeNS((const void*)(ptr),                                                            \
                                        atomic_load_explicit(&snx_tracy_memory_callstack, memory_order_relaxed),       \
                                        (pool)));                                                                      \
    } while (0)
#define SNX_MEM_DISCARD(name) SNX_TRACY_EMIT(TracyCMemoryDiscard(name))
void snx_tracy_frame_mark(void); /* one unnamed frame mark per presented frame, with a fresh baseline per connection */
#define SNX_FRAME_MARK() snx_tracy_frame_mark()
/* A mark in a named frame set, with the same first-mark seed as the unnamed one but per name. The name is copied
   into a table that lives for the capture: Tracy sends its address, which the server reads when a GUI asks. */
void snx_tracy_frame_mark_named(const char* name);
#define SNX_FRAME_MARK_NAMED(name) snx_tracy_frame_mark_named(name)
/* a discontinuous frame: the work to the end of the block, one of a named frame set (the audio tick's, say). A
   set's starts and ends pair and do not nest, and a set is either all continuous or all discontinuous. */
typedef struct {
    int started;
    const char* name;
} SnxTracyFrame;
#define SNX_FRAME_SCOPED(ctx, name)                                                                                    \
    [[gnu::cleanup(snx_tracy_frame_at_scope)]] SnxTracyFrame ctx = {snx_tracy_frame_start(name), (name)}
/* A phase on the section track, ended at the end of the block it opens in, a return included. The start returns the
   pair together: the server sends back no id when it is not connected, and an id of 0 has no end to send. The
   category is announced once at startup; 0 is the uncategorized track. */
SnxTracySection snx_tracy_section_start(const char* name); /* the 0-category form, for callers that do not need a named category */
SnxTracySection snx_tracy_section_start_c(const char* name, uint16_t category);
void snx_tracy_section_setup(uint16_t category, const char* name);
void snx_tracy_section_at_scope(SnxTracySection* s);
#define SNX_SECTION_SCOPED_C(ctx, name, category)                                                                      \
    [[gnu::cleanup(snx_tracy_section_at_scope)]] SnxTracySection ctx =                                                 \
        snx_tracy_section_start_c((name), (uint16_t)(category))
#define SNX_SECTION_SCOPED(ctx, name) SNX_SECTION_SCOPED_C(ctx, name, 0)
#define SNX_PLOT(name, value) SNX_TRACY_EMIT(TracyCPlot(name, value))
/* a plot whose format is sent once per name and connection: type 0 number, 1 memory, 2 percentage, 3 watt. The name
   must be a string literal: the cache keys on its address and holds at most 64 names, so a later name past that cap
   sends no format, though its value still goes out. */
void snx_tracy_plot_cfg(const char* name, int type, int step, int fill, uint32_t color);
#define SNX_PLOT_CFG(name, value, type, step, fill, color)                                                            \
    do {                                                                                                               \
        const char* snx_plot_name_ = (name); /* one evaluation, kept for the config and the plot */                    \
        snx_tracy_plot_cfg(snx_plot_name_, (type), (step), (fill), (color));                                           \
        SNX_TRACY_EMIT(TracyCPlot(snx_plot_name_, (value)));                                                           \
    } while (0)
/* the name goes into the client's table, which the server asks for when a GUI connects; kept per thread, so a
   thread that named itself before the client started is named again when it attaches */
#define SNX_THREAD_NAME(name) snx_tracy_thread_name(name)
/* A name with a group hint for the profiler. Both strings are copied into the thread's state and replayed at each
   new connection, where the C++ bridge applies the hint the client's own way. */
void snx_tracy_thread_name_hint(const char* name, const char* hint);
#define SNX_THREAD_NAME_HINT(name, hint) snx_tracy_thread_name_hint((name), (hint))
void snx_tracy_fiber_enter(uint32_t slot, uint32_t kind, const char* name);
void snx_tracy_fiber_leave(void);
void snx_tracy_fiber_leave_all(void);

/* A frame's picture on the timeline: w x h RGBA8 pixels, at the moment they were read back. The client copies them,
   so the buffer is only read here. Costs a readback of the presented frame, so the caller asks at its own cadence */
void snx_tracy_frame_image(const void* image, uint16_t width, uint16_t height);
/* The GPU track: a context per queue, the zones' query ids paired with their timestamps at the read-back, the two
   clocks' calibration. A query id of 0 means the emission did not go out: the caller keeps the pair and sends only
   what it got. The context is announced at the caller's first read-back (snx_tracy_gpu_context, which returns 1
   then, so the caller can take its calibration anchor); a zone goes out only after its context did. Each
   emission carries the connection it belongs to (*ep at the begin): the client's deferred queue replays the
   contexts at a new connection, and an end or a time from another one than the begin's is dropped, where the
   server would fault on a query it never saw. */
int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated, const char* name);
/* 0 sends a zone without a callstack; above 0 the begin asks Tracy for that many frames too. The GPU thread reads it
   while a slow frame can raise it for a few frames, so it is atomic too. */
extern _Atomic int snx_tracy_gpu_zone_callstack;
uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx, uint64_t* ep);
uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx, uint64_t ep);
void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx, uint64_t ep);
void snx_tracy_gpu_sync(int64_t gpu_time, int ctx);
void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx);
/* The calibration helpers are defined once below both branches: they need SnxTracyGpuCal and the connection id. */
/* The connection's id: a new connection's server replays the deferred contexts, so a calibration anchor and an
   in-flight pair's epoch are keyed on it (both re-anchor at it). A run whose client never started a server, or
   whose server never reconnected, has 1. */
uint64_t snx_tracy_connection_id(void);
/* 1 when the client is up and a server is attached. TracyCIsConnected reads the client's own data, which a run
   that never started one (or one that has shut down) has none of, so the code that asks per frame asks this. */
int snx_tracy_attached(void);
/* SNX_FRAME_SCOPED's pair: 1 when the start went out, so the block's end sends none otherwise */
int snx_tracy_frame_start(const char* name);
void snx_tracy_frame_at_scope(SnxTracyFrame* f);
/* The named locks' events for the profiler's lock pane: the context a PMutex keeps once pm_lock_announce named it.
   The events are single items, each gated on its own, so an unpaired one costs nothing but its absence. */
typedef struct __tracy_lockable_context_data SnxTracyLock;
SnxTracyLock* snx_tracy_lock_make(const char* name);
void snx_tracy_lock_free(SnxTracyLock* l);
void snx_tracy_lock_wait(SnxTracyLock* l);
void snx_tracy_lock_lock(SnxTracyLock* l);
void snx_tracy_lock_unlock(SnxTracyLock* l);
void snx_tracy_lock_try(SnxTracyLock* l, int acquired);
/* The marks name the place in the guest where a mutex was held. Off, neither a mark nor a try event goes out. A trace
   parameter writes this while a server is attached, and the lock call sites read it on other threads, so it is atomic. */
extern _Atomic int snx_tracy_lock_marks;
void snx_tracy_lock_mark(SnxTracyLock* l, const char* file, uint32_t line);
#define SNX_LOCK_MARK(lockctx) snx_tracy_lock_mark((lockctx), __FILE__, (uint32_t)__LINE__)
/* Starts the client when --tracy or --tracy-capture asks for it. Returns 1 when the capture it was asked for is not
   running; the run goes on without one. gate_open is 1 while emissions are allowed: closed before the client starts
   and, again, before it shuts down. tracy_conn reads it so TracyCIsConnected is never reached while the client has
   no data to read (an assert). */
int snx_tracy_gate_open(void);
int snx_tracy_init(const char* title, const SnxTracyHost* host);
/* A console's runtime's start (GBA, DS, 3DS, Vita): the client when its game's settings ask for it (tracy_profiler,
   tracy_capture), its captures in the game's folder, its lines on stderr, which its game log keeps. As init returns */
int snx_tracy_console(const char* folder, const char* name, int headless);
void snx_tracy_thread_name(const char* name);
/* F10: starts the client if needed, then a capture that runs for tracy_capture_seconds. */
void snx_tracy_capture_now(void);
/* 0 warning, 1 log, 2 verbose. */
void snx_tracy_log(int lvl, const char* text);
/* A fatal end of the run: the message goes to the trace with the fatal severity and no runtime lock, so the crash
   path can call it. Tracy still allocates and queues the text, so this is a best effort before exit, not a promise
   that the event reaches disk. Tracy 0.15.0 has no call that writes a trace from the app; closing the client after
   this is what gives a capture the events up to the fatal. */
void snx_tracy_fatal(const char* text);
/* Closes the client cleanly before plat_exit_now. */
void snx_tracy_shutdown(void);
#else
/* Defined by tracy.c in both builds; without a client no parameter callback can change it. */
extern _Atomic int snx_tracy_frame_images_live;
/* The stubs use their arguments through sizeof, which never evaluates them: a value a caller computed for one is still
   a use, so nothing warns, and no call in an argument (a clock read for a plot) runs without the client. */
#define SNX_ZONE_IMPL(ctx, name, color, cs, depth)                                                                     \
    [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(color), (void)sizeof(cs), (void)sizeof(depth), 0)
#define SNX_ZONE(ctx, name)                     [[maybe_unused]] int ctx = ((void)sizeof(name), 0)
#define SNX_ZONE_C(ctx, name, color)            [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(color), 0)
#define SNX_ZONE_CS(ctx, name, depth)           [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(depth), 0)
#define SNX_ZONE_TRANSIENT(ctx, name)           [[maybe_unused]] int ctx = ((void)sizeof(name), 0)
#define SNX_ZONE_TRANSIENT_CS(ctx, name, depth) [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(depth), 0)
#define SNX_ZONE_COLOR(zctx, color)             ((void)sizeof(zctx), (void)sizeof(color))
#define SNX_ZONE_TEXT(ctx, ...)                 ((void)sizeof(ctx), (void)sizeof(#__VA_ARGS__))
#define SNX_ZONE_NAME(zctx, text, size)         ((void)sizeof(zctx), (void)sizeof(text), (void)sizeof(size))
#define SNX_ZONE_NAME_F(zctx, fmt, ...)         ((void)sizeof(zctx), (void)sizeof(fmt))
#define SNX_ZONE_VALUE(ctx, value)              ((void)sizeof(value))
#define SNX_MSG(text, size)                     ((void)sizeof(text), (void)sizeof(size))
#define SNX_LOG(text, size, severity, color, depth)                                                                    \
    ((void)sizeof(text), (void)sizeof(size), (void)sizeof(severity), (void)sizeof(color), (void)sizeof(depth))
#define SNX_MSG_CS(text, size, depth) ((void)sizeof(text), (void)sizeof(size), (void)sizeof(depth))
#define SNX_ZONE_T(ctx, name, ...)    [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(#__VA_ARGS__), 0)
#define SNX_ZONE_V(ctx, name, value)  [[maybe_unused]] int ctx = ((void)sizeof(value), 0)
#define SNX_ZONE_TV(ctx, name, value, ...)                                                                             \
    [[maybe_unused]] int ctx = ((void)sizeof(name), (void)sizeof(value), (void)sizeof(#__VA_ARGS__), 0)
#define SNX_ZONE_END(ctx)        ((void)0)
#define SNX_MEM_ALLOC(ptr, size, pool)   ((void)sizeof(ptr), (void)sizeof(size), (void)sizeof(pool))
#define SNX_MEM_FREE(ptr, pool)          ((void)sizeof(ptr), (void)sizeof(pool))
#define SNX_MEM_ALLOC_D(ptr, size, pool) ((void)sizeof(ptr), (void)sizeof(size), (void)sizeof(pool))
#define SNX_MEM_FREE_D(ptr, pool)        ((void)sizeof(ptr), (void)sizeof(pool))
#define SNX_MEM_DISCARD(name)            ((void)sizeof(name))
#define SNX_FRAME_MARK()        ((void)0)
#define SNX_FRAME_MARK_NAMED(name)       ((void)sizeof(name))
#define SNX_FRAME_SCOPED(ctx, name)      [[maybe_unused]] int ctx = ((void)sizeof(name), 0)
#define SNX_SECTION_SCOPED_C(ctx, name, category)                                                                      \
    [[maybe_unused]] SnxTracySection ctx = ((void)sizeof(name), (void)sizeof(category), (SnxTracySection){0})
#define SNX_SECTION_SCOPED(ctx, name) [[maybe_unused]] SnxTracySection ctx = ((void)sizeof(name), (SnxTracySection){0})
#define SNX_LOCK_MARK(lockctx)        ((void)sizeof(lockctx))
#define SNX_PLOT(name, value)         ((void)sizeof(name), (void)sizeof(value))
#define SNX_PLOT_CFG(name, value, type, step, fill, color)                                                             \
    ((void)sizeof(name), (void)sizeof(value), (void)sizeof(type), (void)sizeof(step), (void)sizeof(fill),              \
     (void)sizeof(color))
#include "common/utilities/common.h"
/* the thread's name for the system: the "threads:" line of the log (main.c), a debugger and a crash report name the
   threads by it */
#define SNX_THREAD_NAME(name) snx_thread_name(name)
#define SNX_THREAD_NAME_HINT(name, hint) snx_tracy_thread_name_hint((name), (hint))
/* the gate: no client, so nothing is in flight and no caller enters one */
static inline int snx_tracy_enter(void) { return 0; }
static inline void snx_tracy_leave(void) {}
static inline int snx_tracy_attached(void) { return 0; }
static inline void snx_tracy_fiber_enter(uint32_t slot, uint32_t kind, const char* name) {
    (void)slot, (void)kind, (void)name;
}
static inline void snx_tracy_fiber_leave(void) {}
static inline void snx_tracy_fiber_leave_all(void) {}
static inline int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated,
                                        const char* name) {
    (void)ctx, (void)gpu_time, (void)period, (void)calibrated, (void)name;
    return 0;
}
static inline uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx, uint64_t* ep) {
    (void)name, (void)color, (void)ctx;
    if (ep) *ep = 0;
    return 0;
}
static inline uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx, uint64_t ep) {
    (void)query, (void)ctx, (void)ep;
    return 0;
}
static inline void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx, uint64_t ep) {
    (void)gpu_time, (void)query, (void)ctx, (void)ep;
}
static inline void snx_tracy_gpu_sync(int64_t gpu_time, int ctx) { (void)gpu_time, (void)ctx; }
static inline void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx) {
    (void)gpu_time, (void)cpu_delta, (void)ctx;
}
static inline void snx_tracy_frame_image(const void* image, uint16_t width, uint16_t height) {
    (void)image, (void)width, (void)height;
}
static inline uint64_t snx_tracy_connection_id(void) { return 1; }
static inline int snx_tracy_gate_open(void) { return 0; }
typedef struct SnxTracyLock SnxTracyLock;
static inline SnxTracyLock* snx_tracy_lock_make(const char* name) {
    (void)name;
    return NULL;
}
static inline void snx_tracy_lock_free(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_wait(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_lock(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_unlock(SnxTracyLock* l) { (void)l; }
static inline void snx_tracy_lock_try(SnxTracyLock* l, int acquired) { (void)l, (void)acquired; }
static inline void snx_tracy_lock_mark(SnxTracyLock* l, const char* file, uint32_t line) {
    (void)l, (void)file, (void)line;
}
static inline SnxTracySection snx_tracy_section_start(const char* name) {
    (void)name;
    return (SnxTracySection){0, 0};
}
static inline SnxTracySection snx_tracy_section_start_c(const char* name, uint16_t category) {
    (void)name, (void)category;
    return (SnxTracySection){0, 0};
}
static inline void snx_tracy_section_setup(uint16_t category, const char* name) { (void)category, (void)name; }
static inline void snx_tracy_section_at_scope(SnxTracySection* s) { (void)s; }
static inline void snx_tracy_thread_name_hint(const char* name, const char* hint) {
    (void)hint;
    snx_thread_name(name);
}
/* No C++ client here, so none of its registrations run. */
static inline void snx_tracy_conn_set_program_name(const char* name) { (void)name; }
static inline void snx_tracy_conn_register_parameters(void) {}
static inline void snx_tracy_conn_register_source_callback(void) {}
static inline void snx_tracy_conn_thread_name_hint(const char* name, const char* hint) { (void)name, (void)hint; }
/* Stubs for tests that link log.c without tracy.c. */
static inline int snx_tracy_init(const char* title, const SnxTracyHost* host) {
    (void)title, (void)host;
    return 0;
}
static inline int snx_tracy_console(const char* folder, const char* name, int headless) {
    (void)folder, (void)name, (void)headless;
    return 0;
}
static inline void snx_tracy_capture_now(void) {}
static inline void snx_tracy_log(int lvl, const char* text) {
    (void)lvl;
    (void)text;
}
static inline void snx_tracy_fatal(const char* text) { (void)text; }
static inline void snx_tracy_shutdown(void) {}
static inline void snx_tracy_source_add(const char* name, const char* bytes, size_t size) {
    (void)name, (void)bytes, (void)size;
}
#endif

/* The caller's anchor: the context announcement's own pair, which a new connection replays. The server measures
   its GPU delta from that pair and takes the CPU delta from the events the caller sends, so both sides must count
   from the same point after a reconnect. */
static inline void snx_tracy_gpu_cal_announce(SnxTracyGpuCal* c, int calibrated, int64_t cpu, int64_t gpu) {
    c->calibrated = calibrated;
    c->cpu_anchor = calibrated ? cpu : 0;
    c->cpu_prev = c->cpu_anchor;
    c->gpu_anchor = calibrated ? gpu : 0;
    c->gpu_prev = c->gpu_anchor;
    c->conn = snx_tracy_connection_id();
}

/* A new connection's server re-anchored at the replayed context's original pair, so the CPU delta is measured
   from the announcement again. The caller asks at each read-back: the compare is the anchor's, and no caller
   keeps a connection of its own for it. */
static inline void snx_tracy_gpu_cal_reconnect(SnxTracyGpuCal* c) {
    uint64_t conn = snx_tracy_connection_id();
    if (conn == c->conn) return;
    c->conn = conn;
    c->cpu_prev = c->cpu_anchor;
    c->gpu_prev = c->gpu_anchor;
}

static inline int64_t snx_tracy_gpu_cal_delta(SnxTracyGpuCal* c, int64_t cpu, int64_t gpu) {
    int64_t d;
    if (!c->calibrated) return 0;
    if (!c->cpu_prev || !c->gpu_prev) { /* no anchor was taken: the next pair starts one */
        c->cpu_prev = cpu;
        c->gpu_prev = gpu;
        return 0;
    }
    d = cpu - c->cpu_prev;
    /* the server divides by the device delta, so a clock that does not advance sends none */
    if (d <= 0 || gpu <= c->gpu_prev) return 0;
    c->cpu_prev = cpu;
    c->gpu_prev = gpu;
    return d;
}

#ifdef __cplusplus
}
#endif

#endif
