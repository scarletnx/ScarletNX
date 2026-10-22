/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* host1x.c's channels with nvdrv, the engines and the platform as stand-ins: MAP_BUFFER's IOVAs (one per handle and
   buffer, a guard page between, refused for a dead or empty handle, freed by nvmap's FREE, 1024 at most), iova_ptr's
   bounds, GET_SYNCPOINT, SUBMIT's validation and promised fences, the command stream reaching the engine (and not
   past its registers), a host class wait holding a job back, and a close that runs what was queued first */
#include "nx/gpu/video/host1x.c"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---- the platform: real locks and threads */
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_destroy(PMutex* m) { pthread_mutex_destroy(&m->l); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) { pthread_cond_init(&c->c, NULL); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
static void deadline(struct timespec* t, i64 ns) {
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += (time_t)(ns / 1000000000);
    t->tv_nsec += (long)(ns % 1000000000);
    if (t->tv_nsec >= 1000000000) t->tv_sec++, t->tv_nsec -= 1000000000;
}
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    struct timespec t;
    if (ns < 0) return pthread_cond_wait(&c->c, &m->l) != 0;
    deadline(&t, ns);
    return pthread_cond_timedwait(&c->c, &m->l, &t) != 0;
}
/* host1x_drain's clock, which nothing here calls */
u64 plat_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000u + (u64)t.tv_nsec;
}
void plat_sleep_ns(i64 ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&t, NULL);
}
typedef struct {
    PThreadFn fn;
    void* arg;
} Start;
static void* start(void* p) {
    Start s = *(Start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    Start* s = malloc(sizeof *s);
    (void)stack;
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&t, NULL, start, s)) return -1;
    pthread_detach(t);
    return 0;
}
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    exit(1);
}
u8* g_mem;

/* ---- nvmap: handles 1..2047, alive unless dead[h], at 0x1000 + 0x100 * (h % 32), size[h] or 0x100 */
#define HANDLES 2048
static int dead[HANDLES];
static u32 size_of[HANDLES];
int nvmap_get(u32 h, u64* addr, u32* size) {
    int alive = h && h < HANDLES && !dead[h];
    *addr = alive ? 0x1000 + 0x100ull * (h % 32) : 0;
    *size = alive ? (size_of[h] ? size_of[h] : 0x100) : 0;
    if (alive && size_of[h] == ~0u) *size = 0;
    return alive;
}

/* ---- syncpoints */
static pthread_mutex_t sp_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sp_cv = PTHREAD_COND_INITIALIZER;
static u32 sp_val[256], sp_max[256], sp_next = 20, sp_freed;
u32 syncpt_alloc(void) { return sp_next++; }
void syncpt_free(u32 id) {
    (void)id;
    sp_freed++;
}
u32 syncpt_read(u32 id) {
    u32 v;
    pthread_mutex_lock(&sp_lk);
    v = sp_val[id & 255];
    pthread_mutex_unlock(&sp_lk);
    return v;
}
void syncpt_add(u32 id, u32 n) {
    pthread_mutex_lock(&sp_lk);
    sp_val[id & 255] += n;
    pthread_cond_broadcast(&sp_cv);
    pthread_mutex_unlock(&sp_lk);
}
u32 syncpt_promise(u32 id, u32 n) {
    u32 v;
    pthread_mutex_lock(&sp_lk);
    v = sp_max[id & 255] += n;
    pthread_mutex_unlock(&sp_lk);
    return v;
}
int syncpt_wait(u32 id, u32 thresh, i64 ns) {
    struct timespec t;
    int ok;
    deadline(&t, ns);
    pthread_mutex_lock(&sp_lk);
    while (!(ok = (i32)(sp_val[id & 255] - thresh) >= 0))
        if (pthread_cond_timedwait(&sp_cv, &sp_lk, &t)) break;
    pthread_mutex_unlock(&sp_lk);
    return ok;
}

/* ---- the engines */
struct NvdecCtx {
    int jobs;
    u32 app;
};
static int nvdec_alive, vic_jobs;
NvdecCtx* nvdec_new(void) {
    nvdec_alive++;
    return calloc(1, sizeof(NvdecCtx));
}
void nvdec_free(NvdecCtx* d) {
    if (d) nvdec_alive--;
    free(d);
}
void nvdec_execute(NvdecCtx* d, const u32* r) {
    d->jobs++;
    d->app = r[0x200 / 4];
}
void vic_execute(const u32* r) {
    (void)r;
    vic_jobs++;
}

/* ---- ioctls */
static u8 buf[0x400];
static void put(u32 off, u32 v) { snx_st_le32(buf + off, v); }
static u32 map(H1Ch* ch, u32 h, u32* iova) {
    u32 r;
    memset(buf, 0, sizeof buf);
    put(0, 1);
    put(12, h);
    put(16, 0xDEAD);
    r = host1x_ioctl(ch, 0x09, buf, 20);
    *iova = rd32(buf, 16);
    return r;
}
static u32 get_syncpoint(H1Ch* ch, u32 idx) {
    memset(buf, 0, sizeof buf);
    put(0, idx);
    CHECK(host1x_ioctl(ch, 0x02, buf, 8) == 0);
    return rd32(buf, 4);
}

static void iova_tests(H1Ch* ch) {
    u32 a, a2, b, c, r, h, iovas[IOVA_SLOTS];
    CHECK(map(ch, 1, &a) == 0 && a == IOVA_BASE);
    CHECK(map(ch, 1, &a2) == 0 && a2 == a); /* the same buffer keeps its IOVA */
    size_of[2] = 0x20000;
    CHECK(map(ch, 2, &b) == 0 && b == a + 2 * IOVA_PAGE); /* a guard page past each mapping */
    CHECK(iova_ptr(a, 0x100) == g_mem + 0x1100);
    CHECK(iova_ptr(a + 0xFF, 1) == g_mem + 0x11FF);
    CHECK(!iova_ptr(a + 0xFF, 2) && !iova_ptr(a + 0x100, 1) && !iova_ptr(a, 0) && !iova_ptr(a - 1, 1));
    CHECK(!iova_ptr(a + 1, ~0ull)); /* a size that wraps */
    dead[3] = 1;
    CHECK(map(ch, 3, &c) == 4 && c == 0); /* no made-up size for a dead handle */
    size_of[4] = ~0u;
    CHECK(map(ch, 4, &c) == 4);
    /* two entries, the first failing: the second still maps, the first error is the result */
    memset(buf, 0, sizeof buf);
    put(0, 2);
    put(12, 3);
    put(20, 6);
    CHECK(host1x_ioctl(ch, 0x09, buf, 28) == 4 && rd32(buf, 16) == 0 && rd32(buf, 24) == b + 0x30000);
    host1x_forget_handle(1);
    CHECK(!iova_ptr(a, 1));
    CHECK(map(ch, 7, &c) == 0 && c == a); /* the freed range is used again */
    host1x_forget_handle(7);
    host1x_forget_handle(2);
    host1x_forget_handle(6);
    CHECK(g_niova == 0);
    dead[3] = 0, size_of[2] = size_of[4] = 0;
    for (h = 1; h <= IOVA_SLOTS; h++) CHECK(map(ch, h, &iovas[h - 1]) == 0);
    CHECK(map(ch, IOVA_SLOTS + 1, &c) == 6);
    for (h = 1; h < IOVA_SLOTS; h++) CHECK(iovas[h] == iovas[h - 1] + 2 * IOVA_PAGE);
    for (h = 1; h <= IOVA_SLOTS; h++) host1x_forget_handle(h);
    CHECK(g_niova == 0);
    memset(buf, 0, sizeof buf); /* UNMAP_BUFFER keeps the mapping */
    r = map(ch, 9, &c);
    CHECK(r == 0);
    put(0, 1);
    put(12, 9);
    CHECK(host1x_ioctl(ch, 0x0A, buf, 20) == 0 && iova_ptr(c, 1));
    host1x_forget_handle(9);
}

/* a submit of one command buffer (handle 10 at 0x1000 + 0x140, its words) and the increments (id, count) */
static u32 submit_job(H1Ch* ch, const u32* words, u32 nwords, u32 declared, const u32* inc, u32 ninc, u32 nfe,
                      u32* fences) {
    u32 i, o = 16, r;
    for (i = 0; i < nwords; i++) snx_st_le32(g_mem + 0x1000 + 0x100 * 10 + 4 * i, words[i]);
    size_of[10] = 0x100;
    memset(buf, 0, sizeof buf);
    put(0, 1);
    put(8, ninc);
    put(12, nfe);
    put(o, 10);
    put(o + 4, 0);
    put(o + 8, declared);
    o += 12;
    for (i = 0; i < ninc; i++, o += 20) {
        put(o, inc[2 * i]);
        put(o + 4, inc[2 * i + 1]);
    }
    r = host1x_ioctl(ch, 0x01, buf, o + 4 * nfe);
    for (i = 0; i < nfe; i++) fences[i] = rd32(buf, o + 4 * i);
    return r;
}

static void channel_tests(void) {
    H1Ch* ch = host1x_open(0);
    u32 s0, s1, f[2], inc[4];
    /* an engine method selected, written, then the execute method: and one past the registers, ignored */
    const u32 job[] = {1u << 28 | 0x10 << 16 | 2, 0x200 / 4,         3, 1u << 28 | 0x10 << 16 | 2, 0x300 / 4, 0,
                       1u << 28 | 0x10 << 16 | 2, 0x400 + 0x300 / 4, 0};
    /* the host class: a payload of 5, then a wait for s1 to reach it; back to NVDEC, an execute */
    u32 wait[] = {0x4E << 16 | 0x01 << 6 | 1, 5, 4u << 28 | 0x50 << 16 | 0, 0xF0 << 6, 1u << 28 | 0x10 << 16 | 2,
                  0x300 / 4, 0};
    CHECK(ch && nvdec_alive == 1);
    iova_tests(ch);
    memset(buf, 0, sizeof buf);
    put(0, 16);
    CHECK(host1x_ioctl(ch, 0x02, buf, 8) == 4);
    s0 = get_syncpoint(ch, 0);
    CHECK(s0 >= 20 && get_syncpoint(ch, 0) == s0);
    s1 = get_syncpoint(ch, 1);
    wait[2] |= s1;
    CHECK(s1 != s0);
    CHECK(host1x_ioctl(ch, 0x01, buf, 8) == 4); /* shorter than its header */
    memset(buf, 0, sizeof buf);
    put(0, 100); /* command buffers past the buffer */
    CHECK(host1x_ioctl(ch, 0x01, buf, 64) == 4);
    inc[0] = 99, inc[1] = 1;
    CHECK(submit_job(ch, job, 9, 9, inc, 1, 1, f) == 4 && sp_max[99] == 0); /* not the channel's syncpoint */
    inc[0] = s0, inc[1] = 2, inc[2] = s0, inc[3] = 1;
    CHECK(submit_job(ch, job, 9, 9, inc, 2, 2, f) == 0 && f[0] == 2 && f[1] == 3);
    CHECK(syncpt_wait(s0, 3, 1000000000));
    CHECK(ch->dec->jobs == 1 && ch->dec->app == 3);
    /* a command buffer longer than its handle is skipped, its increments still happen */
    inc[1] = 1;
    CHECK(submit_job(ch, job, 9, 0x41, inc, 1, 1, f) == 0 && f[0] == 4);
    CHECK(syncpt_wait(s0, 4, 1000000000) && ch->dec->jobs == 1);
    /* the wait holds the job back until s1 reaches 5 */
    CHECK(submit_job(ch, wait, 7, 7, inc, 1, 1, f) == 0 && f[0] == 5);
    CHECK(!syncpt_wait(s0, 5, 50000000) && ch->dec->jobs == 1);
    syncpt_add(s1, 5);
    CHECK(syncpt_wait(s0, 5, 1000000000) && ch->dec->jobs == 2);
    /* queued work runs before the close lets the channel go */
    CHECK(submit_job(ch, job, 9, 9, inc, 1, 0, f) == 0);
    host1x_close(ch);
    CHECK(sp_val[s0] == 6 && sp_freed == 2 && nvdec_alive == 0);
    ch = host1x_open(1);
    CHECK(ch && nvdec_alive == 0);
    s0 = get_syncpoint(ch, 0);
    inc[0] = s0;
    CHECK(submit_job(ch, job, 9, 9, inc, 1, 0, f) == 0);
    host1x_close(ch);
    CHECK(vic_jobs == 1 && sp_freed == 3);
    host1x_close(NULL);
}

int main(void) {
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    channel_tests();
    free(g_mem);
    printf("host1x_channel: %u checks passed\n", checks);
    return 0;
}
