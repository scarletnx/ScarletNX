/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The audio clock (audio.c's audio_clock) and the renderer's frames it queues (audren.c's audren_tick) on a host this
   test makes: a clock that moves only in the clock thread's sleeps, which a stall can lengthen, and an output that
   takes a buffer of sound at a time as a device does. What it checks is how far the sound trails the clock, the
   sound the host still holds when a frame comes, after the host stalled */
#include "nx/hle/services/services_int.h"
#include <setjmp.h>
#include <stdarg.h>

#include "nx/hle/services/audio.c"
#include "nx/hle/services/audren.c"
#include "nx/hle/hle.c"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "audio_clock.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

#define TEST_MS         1000000ull
#define TEST_MS_BYTES   192u /* 48 kHz stereo s16 */
#define TEST_STALLS_MAX 20

/* the output: it takes a buffer of sound every period, and plays silence for what it does not find */
struct PAudio {
    u64 next;   /* its next take */
    u64 period; /* ns between takes */
    u32 buffer; /* bytes a take */
    u32 queued;
    u64 silent, given; /* bytes */
    u64 queues;        /* the frames it was given, whole or played shorter */
    int started;
};
/* the host: its time, the stalls to come (the sleep that passes at ends late ms later; whole: the output's own
   thread stalls with it, and takes nothing meanwhile), when the run ends, what the clock's frames found since the
   last look, and its output */
static struct {
    u64 now, end;
    struct {
        u64 at;
        u32 late_ms;
        int whole;
    } stalls[TEST_STALLS_MAX];
    unsigned nstalls, stall;
    jmp_buf ended;
    u32 least, most; /* the bytes the output held at a frame */
    unsigned frames, events, checks;
    PAudio out;
} g_test = {.out = {.period = 20 * TEST_MS, .buffer = 20 * TEST_MS_BYTES}};

static void out_run(PAudio* a) {
    while (a->started && a->next <= g_test.now) {
        u32 take = a->queued < a->buffer ? a->queued : a->buffer;
        a->queued -= take;
        a->silent += a->buffer - take;
        a->next += a->period;
    }
}
PAudio* plat_audio_open(u32 rate, u32 channels) {
    CHECK(rate == 48000 && channels == 2);
    return &g_test.out;
}
void plat_audio_close(PAudio* a) { (void)a; }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    (void)samples;
    out_run(a);
    a->queued += bytes;
    a->given += bytes;
    a->queues++;
    return 1;
}
void plat_audio_start(PAudio* a, int started) {
    a->started = started;
    a->next = g_test.now + a->period;
}
u32 plat_audio_queued(PAudio* a) {
    out_run(a);
    if (a->started) {
        if (!g_test.frames || a->queued < g_test.least) g_test.least = a->queued;
        if (a->queued > g_test.most) g_test.most = a->queued;
        g_test.frames++;
    }
    return a->queued;
}
void plat_audio_volume(PAudio* a, float volume) { (void)a, (void)volume; }

u64 plat_ns(void) { return g_test.now; }
u64 guest_ns(void) { return g_test.now; }
void plat_sleep_ns(i64 ns) {
    CHECK(ns > 0);
    g_test.now += (u64)ns;
    if (g_test.stall < g_test.nstalls && g_test.stalls[g_test.stall].at <= g_test.now) {
        PAudio* a = &g_test.out;
        out_run(a);
        g_test.now += g_test.stalls[g_test.stall].late_ms * TEST_MS;
        if (g_test.stalls[g_test.stall++].whole) /* the takes it missed are silence, and it goes on from now */
            for (; a->next <= g_test.now; a->next += a->period) a->silent += a->buffer;
    }
    if (g_test.now >= g_test.end) longjmp(g_test.ended, 1);
}

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    return 0;
}
void snx_thread_name(const char* name) { (void)name; }
void logf_(int level, const char* format, ...) { (void)level, (void)format; }
void fatal(const char* fmt, ...) {
    fprintf(stderr, "audio_clock.c: unexpected fatal: %s\n", fmt);
    exit(1);
}
void kernel_dump_threads(void) {}
void mem_query(u64 addr, MemBlock* b) {
    (void)addr;
    memset(b, 0, sizeof *b);
}
/* the renderers' event, signalled every frame */
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    kobj_set_name(&e->o, name);
    return e;
}
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) { (void)o; }
void kevent_signal(KEvent* e) {
    (void)e;
    g_test.events++;
}
u32 owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}
/* the audio outs and the hooks' output streams, which have their own tests: none here */
void nnaudio_out_tick(u64 now) { (void)now; }
void nnaudio_out_state_drop(void) {}
void nnaudio_out_state_rebuild(void) {}
void outstream_tick(u64 now) { (void)now; }
/* aud:u's objects, which this test never makes */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    (void)name, (void)dispatch;
    return NULL;
}
void svcobj_unref(SvcObj* obj) { (void)obj; }
void out_obj(IpcCtx* c, SvcObj* obj) { (void)c, (void)obj; }
void out_copy_h(IpcCtx* c, u32 handle) { (void)c, (void)handle; }
void ipc_out_overflow(IpcCtx* c) { (void)c; }
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }
/* the DSP: a state with a rate and a frame of 5 ms, and silence */
ARState* audren_state_new(const u8* params) {
    ARState* s = calloc(1, sizeof *s);
    (void)params;
    CHECK(s != NULL);
    s->rate = 48000;
    s->frame = 240;
    return s;
}
void audren_state_free(ARState* s) { free(s); }
int audren_state_update(ARState* s, const u8* in, u64 in_size, u8* out, u64 out_size) {
    (void)s, (void)in, (void)in_size, (void)out, (void)out_size;
    return 0;
}
int audren_revision(u32 rev) { return (int)rev; }
int audren_frame(ARState* s, i16* stereo) {
    memset(stereo, 0, (size_t)s->frame * 4);
    return 1;
}

/* the clock's thread until the host's time is at_ms: what its frames found in that time, in ms of sound, the silence
   the output played for want of sound, the frames it was not given and the sound the frames it was given were played
   shorter by. Its turn starts at now again in each call, as
   after a pause, so a call ends where no stall is */
typedef struct {
    u32 least, most, silent;
    unsigned dropped;
    u32 shorter; /* the sound the frames given were played shorter by, in tenths of a ms */
} Found;
static Found run_until(u64 at_ms) {
    u64 silent = g_test.out.silent, given = g_test.out.given, queues = g_test.out.queues;
    unsigned events = g_test.events;
    g_test.end = at_ms * TEST_MS;
    g_test.frames = 0;
    g_test.least = g_test.most = 0;
    if (!setjmp(g_test.ended)) audio_clock(NULL);
    u64 frames = g_test.out.queues - queues, full = frames * 5 * TEST_MS_BYTES;
    Found f = {g_test.least / TEST_MS_BYTES, g_test.most / TEST_MS_BYTES,
               (u32)((g_test.out.silent - silent) / TEST_MS_BYTES), g_test.events - events - (unsigned)frames,
               (u32)((full - (g_test.out.given - given)) * 10 / TEST_MS_BYTES)};
    return f;
}
static void stall_of(u64 at_ms, u32 late_ms, int whole) {
    CHECK(g_test.nstalls < TEST_STALLS_MAX);
    g_test.stalls[g_test.nstalls].at = at_ms * TEST_MS;
    g_test.stalls[g_test.nstalls].late_ms = late_ms;
    g_test.stalls[g_test.nstalls++].whole = whole;
}
static void stall(u64 at_ms, u32 late_ms) { stall_of(at_ms, late_ms, 0); }

int main(void) {
    static const u8 params[0x34];
    Found f;
    hle_init();
    audio_init();
    u32 id = audren_open(params, 0);
    CHECK(id != 0);
    audren_at(id)->state = SNX_AUDREN_STARTED;
    /* a host that never stalls: the output takes 20 ms as the clock gives the fourth frame of 5, so a frame finds
       0 to 15 ms, every frame is played and there is no silence; the game's event every frame */
    f = run_until(10000);
    CHECK(f.least == 0 && f.most == 15 && !f.silent && !f.dropped && !f.shorter && g_test.events == 2000);
    CHECK(g_test.out.given == 10000 * TEST_MS_BYTES);
    /* a turn of the clock's thread 12 ms late: within what the output holds between two takes */
    stall(11000, 12);
    f = run_until(20000);
    CHECK(f.least == 0 && f.most == 15 && !f.silent && !f.dropped && !f.shorter);
    /* 40 ms late: the output plays 40 ms of silence, then the clock gives the stall's eight frames at once, and from
       there a frame finds 40 ms more than before: the sound trails the clock by the stall */
    stall(21000, 40);
    f = run_until(22000);
    CHECK(f.silent == 40 && !f.dropped);
    f = run_until(30000);
    CHECK(f.least == 40 && f.most == 55 && !f.silent && !f.dropped && !f.shorter);
    /* which is taken back once no frame found the output empty for 10 s, by playing frames shorter, never by dropping
       one: at most 0.5%, less as it nears the target of 20 ms. 18.5 of the 20 ms in the next 10 s, the rest in the 20
       after, and no further */
    f = run_until(40000);
    CHECK(f.least == 21 && f.most == 55 && !f.silent && !f.dropped && f.shorter == 185);
    f = run_until(60000);
    CHECK(f.least == 20 && f.most == 36 && !f.silent && !f.dropped && f.shorter == 15);
    /* a host that goes on stalling, 40 ms every 5 s: the first stall's silence is the 20 ms the output did not hold,
       and the 40 ms it then holds stay, each stall emptying it and none of them heard */
    for (u64 at = 61000; at < 100000; at += 5000) stall(at, 40);
    f = run_until(62000);
    CHECK(f.silent == 20 && !f.dropped);
    f = run_until(101000);
    CHECK(f.least == 0 && f.most == 55 && !f.silent && !f.dropped && !f.shorter && g_test.stall == g_test.nstalls);
    /* more than ten frames late, 60 ms: the clock starts over from now and the stall's frames are never made */
    stall(102000, 60);
    f = run_until(103000);
    CHECK(f.silent == 20 && !f.dropped && g_test.events == 103000 / 5 - 12);
    f = run_until(120000);
    CHECK(f.least == 0 && f.most == 15 && !f.silent && !f.dropped && !f.shorter);
    /* an output a thousandth slower than the clock holds a millisecond more each second: played shorter by as much,
       100 ms in 100 s once it settled, 2 ms over the target where the frames' 0.1% shorter keeps up with it, long
       before the bound of 200 ms */
    g_test.out.period += g_test.out.period / 1000;
    f = run_until(220000);
    CHECK(f.least == 0 && f.most == 42 && !f.silent && !f.dropped && f.shorter == 772);
    f = run_until(320000);
    CHECK(f.least == 22 && f.most == 42 && !f.silent && !f.dropped && f.shorter == 999);
    /* the whole process stalls for 30 ms, the output's thread with the clock's: the output missed a take of what it
       held, so the stall's frames leave 20 ms more than before, and a second stall 20 ms more again. No frame found
       it empty, so they are taken back from the next second on, played shorter */
    g_test.out.period = 20 * TEST_MS;
    stall_of(321000, 30, 1);
    stall_of(323000, 30, 1);
    f = run_until(321500);
    CHECK(f.silent == 20 && !f.dropped && f.shorter == 9);
    f = run_until(322000);
    CHECK(f.least == 41 && f.most == 56 && !f.silent && !f.dropped && f.shorter == 2);
    f = run_until(323500);
    CHECK(f.silent == 20 && !f.dropped && f.shorter == 68);
    f = run_until(324000);
    CHECK(f.least == 52 && f.most == 69 && !f.silent && !f.dropped && f.shorter == 20);
    f = run_until(330000);
    CHECK(f.least == 27 && !f.silent && !f.dropped && f.shorter == 251);
    f = run_until(340000);
    CHECK(f.least == 20 && f.most == 42 && !f.silent && !f.dropped && f.shorter == 70 &&
          g_test.stall == g_test.nstalls);
    printf("audio_clock: %u checks passed\n", g_test.checks);
    return 0;
}
