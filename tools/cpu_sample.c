/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux preload sampler. Flat, shared rings survive _exit; the Python parent recovers their final records. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define SAMPLE_MAGIC 0x534e5853414d5031ull
#define SAMPLE_THREADS 4096u
typedef struct { uint64_t ns, pc; uint32_t tid, weight; } Sample;
typedef struct {
    uint64_t magic;
    uint32_t version, capacity, stride, count, error, ready;
    uint64_t poll_ns, interval_ns;
    uint32_t collector_tid, reserved;
    uint64_t state_offset;
} Header;
typedef struct {
    unsigned head, peak;
    uint64_t attempted, dropped, overruns;
    char pad[32];
} Producer;
typedef struct { unsigned tail; char pad[60]; } Consumer;
typedef struct {
    Producer producer;
    Consumer consumer;
    timer_t timer;
    uint32_t tid, registered;
    Sample samples[];
} ThreadSamples;
typedef char sample_size[sizeof(Sample) == 24 ? 1 : -1];
typedef char header_size[sizeof(Header) == 64 ? 1 : -1];
typedef char producer_size[sizeof(Producer) == 64 ? 1 : -1];
typedef char consumer_size[sizeof(Consumer) == 64 ? 1 : -1];
typedef char thread_layout[offsetof(ThreadSamples, samples) == 144 && offsetof(ThreadSamples, tid) == 136 ? 1 : -1];
typedef struct { void* (*fn)(void*); void* argument; } Start;
typedef int (*Create)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
static struct {
    int fd, started, enabled;
    unsigned capacity;
    Header* header;
    char* states;
    Create create;
    pthread_t collector;
    struct sigaction prior;
} g_sample = {.fd = -1};
static __thread struct { ThreadSamples* samples; int* error; } t_sample
    __attribute__((tls_model("initial-exec")));

static ThreadSamples* thread_at(unsigned i) { return (ThreadSamples*)(g_sample.states + (size_t)i * g_sample.header->stride); }
static void report_error(int error) { __atomic_store_n(&g_sample.header->error, (unsigned)error, __ATOMIC_RELAXED); }
static void sample_signal(int signal, siginfo_t* info, void* context) {
    (void)signal;
    ThreadSamples* s = t_sample.samples;
    if (!g_sample.enabled || !s) return;
    int saved_errno = *t_sample.error;
    unsigned head = s->producer.head;
    unsigned used = head - __atomic_load_n(&s->consumer.tail, __ATOMIC_ACQUIRE);
    ++s->producer.attempted;
    if (used == g_sample.capacity) {
        ++s->producer.dropped;
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ucontext_t* cpu = context;
#if defined(__x86_64__)
        uintptr_t pc = (uintptr_t)cpu->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
        uintptr_t pc = (uintptr_t)cpu->uc_mcontext.pc;
#else
#error This sampler needs the host instruction pointer from ucontext_t
#endif
        unsigned overrun = info->si_overrun > 0 ? (unsigned)info->si_overrun : 0;
        s->producer.overruns += overrun;
        s->samples[head & (g_sample.capacity - 1)] = (Sample){
            (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec, pc, s->tid, overrun + 1};
        if (used + 1 > s->producer.peak) s->producer.peak = used + 1;
        __atomic_store_n(&s->producer.head, head + 1, __ATOMIC_RELEASE);
    }
    *t_sample.error = saved_errno;
}
static void write_samples(const Sample* samples, unsigned count) {
    const char* bytes = (const char*)samples;
    size_t left = (size_t)count * sizeof *samples;
    while (left) {
        ssize_t n = write(g_sample.fd, bytes, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { report_error(errno ? errno : EIO); return; }
        bytes += (size_t)n;
        left -= (size_t)n;
    }
}
static void set_timer(ThreadSamples* s, int enabled) {
    struct itimerspec interval = {0};
    if (enabled) {
        interval.it_interval = (struct timespec){(time_t)(g_sample.header->interval_ns / 1000000000),
                                               (long)(g_sample.header->interval_ns % 1000000000)};
        interval.it_value = interval.it_interval;
    }
    if (timer_settime(s->timer, 0, &interval, NULL)) report_error(errno);
}
static void* collect(void* unused) {
    (void)unused;
    __atomic_store_n(&g_sample.header->collector_tid, (unsigned)syscall(SYS_gettid), __ATOMIC_RELEASE);
    struct timespec interval = {(time_t)(g_sample.header->poll_ns / 1000000000),
                                (long)(g_sample.header->poll_ns % 1000000000)};
    for (;;) {
        unsigned threads = __atomic_load_n(&g_sample.header->count, __ATOMIC_ACQUIRE);
        if (threads > SAMPLE_THREADS) threads = SAMPLE_THREADS;
        for (unsigned i = 0; i < threads; ++i) {
            ThreadSamples* s = thread_at(i);
            if (!__atomic_load_n(&s->registered, __ATOMIC_ACQUIRE)) continue;
            unsigned tail = s->consumer.tail;
            unsigned head = __atomic_load_n(&s->producer.head, __ATOMIC_ACQUIRE);
            unsigned count = head - tail, at = tail & (g_sample.header->capacity - 1);
            if (!count) continue;
            unsigned first = count < g_sample.header->capacity - at ? count : g_sample.header->capacity - at;
            write_samples(s->samples + at, first);
            if (count > first) write_samples(s->samples, count - first);
            __atomic_store_n(&s->consumer.tail, head, __ATOMIC_RELEASE);
        }
        nanosleep(&interval, NULL);
    }
    return NULL;
}
static ThreadSamples* arm(void) {
    if (!g_sample.started) return NULL;
    unsigned slot = __atomic_fetch_add(&g_sample.header->count, 1, __ATOMIC_RELAXED);
    if (slot >= SAMPLE_THREADS) { report_error(EOVERFLOW); return NULL; }
    ThreadSamples* s = thread_at(slot);
    s->tid = (uint32_t)syscall(SYS_gettid);
    struct sigevent event = {.sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGPROF};
    event._sigev_un._tid = (int)s->tid;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &s->timer)) { report_error(errno); return NULL; }
    t_sample.error = &errno;
    t_sample.samples = s;
    __atomic_store_n(&s->registered, 1, __ATOMIC_RELEASE);
    set_timer(s, 1);
    return s;
}
static void finish_thread(void* data) {
    ThreadSamples* s = data;
    if (!s) return;
    timer_delete(s->timer);
}
static void* launch(void* argument) {
    Start start = *(Start*)argument;
    free(argument);
    ThreadSamples* samples = arm();
    void* result;
    pthread_cleanup_push(finish_thread, samples);
    result = start.fn(start.argument);
    pthread_cleanup_pop(1);
    return result;
}
static Create resolve_create(void) {
    void* symbol = dlsym(RTLD_NEXT, "pthread_create");
    Create create;
    typedef char pointer_size[sizeof symbol == sizeof create ? 1 : -1];
    (void)sizeof(pointer_size);
    memcpy(&create, &symbol, sizeof symbol);
    __atomic_store_n(&g_sample.create, create, __ATOMIC_RELEASE);
    return create;
}
int pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*fn)(void*), void* argument) {
    Create create = __atomic_load_n(&g_sample.create, __ATOMIC_ACQUIRE);
    if (!create) create = resolve_create();
    if (!__atomic_load_n(&g_sample.started, __ATOMIC_ACQUIRE)) return create(thread, attributes, fn, argument);
    Start* start = malloc(sizeof *start);
    if (!start) { report_error(ENOMEM); return create(thread, attributes, fn, argument); }
    *start = (Start){fn, argument};
    int error = create(thread, attributes, launch, start);
    if (error) free(start);
    return error;
}
static uint64_t number(const char* key, uint64_t fallback) {
    const char* value = getenv(key);
    if (!value || !*value) return fallback;
    char* end;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 10);
    return !errno && !*end ? n : 0;
}
static void fork_child(void) {
    g_sample.started = g_sample.enabled = 0;
    t_sample.samples = NULL;
    sigaction(SIGPROF, &g_sample.prior, NULL);
}
__attribute__((constructor)) static void sample_start(void) {
    Create create = resolve_create();
    const char* path = getenv("SNX_SAMPLE_PATH");
    if (!path || number("SNX_SAMPLE_PARENT_PID", (uint64_t)getppid()) != (uint64_t)getppid()) return;
    uint64_t capacity = number("SNX_SAMPLE_CAPACITY", 64);
    uint64_t poll = number("SNX_SAMPLE_POLL_NS", 2000000), period = number("SNX_SAMPLE_INTERVAL_NS", 1000000);
    if (!capacity || capacity > 65536 || (capacity & (capacity - 1)) ||
        !poll || poll > 100000000 || period < 100000 || period > 1000000000) return;
    g_sample.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (g_sample.fd < 0) return;
    char* name = malloc(strlen(path) + 7);
    if (!name) return;
    strcpy(name, path); strcat(name, ".state");
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    free(name);
    if (fd < 0) return;
    size_t stride = (sizeof(ThreadSamples) + (size_t)capacity * sizeof(Sample) + 63) & ~(size_t)63;
    size_t size = sizeof(Header) + SAMPLE_THREADS * stride;
    void* map = MAP_FAILED;
    if (!ftruncate(fd, (off_t)size)) map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    g_sample.header = map;
    *g_sample.header = (Header){SAMPLE_MAGIC, 1, (uint32_t)capacity, (uint32_t)stride, 0, 0, 0,
                                poll, period, 0, 0, sizeof(Header)};
    g_sample.states = (char*)map + sizeof(Header);
    if (sigaction(SIGPROF, NULL, &g_sample.prior) ||
        (g_sample.prior.sa_handler != SIG_DFL && g_sample.prior.sa_handler != SIG_IGN)) { report_error(EBUSY); return; }
    struct sigaction action = {.sa_sigaction = sample_signal, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, NULL)) { report_error(errno); return; }
    if (pthread_atfork(NULL, NULL, fork_child)) { report_error(EAGAIN); return; }
    g_sample.capacity = (unsigned)capacity;
    g_sample.started = g_sample.enabled = 1;
    arm();
    int error = create(&g_sample.collector, NULL, collect, NULL);
    if (error) { g_sample.started = g_sample.enabled = 0; report_error(error); return; }
    __atomic_store_n(&g_sample.header->ready, 1, __ATOMIC_RELEASE);
    /* The mapping and rings have process lifetime. The parent reads them after waitpid, including after _exit or
       a signal. No exit hook, allocation, file I/O or shared counter belongs in the sampling signal handler. */
}
