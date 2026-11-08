/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit bridge's EGL, GLES and ANativeWindow (arm32.h): the 64-bit bridge's handlers (hle_egl.c, hle_gles.c,
   gles2*.c, hle_android.c), called with an AArch64 CPU of the call's arguments as AAPCS64 passes them: integers in x0
   to x7, floats in v0 to v7, then 8-byte stack slots. A guest pointer becomes a host pointer. The host's EGL objects
   and windows are host pointers, so the guest gets a 32-bit handle for each. A call whose arguments hold pointers in
   memory (glShaderSource's strings, eglChooseConfig's configs) is converted on its own. libandroid's looper, input
   queue, configuration and sensors are the 32-bit bridge's own, at the end */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/endian.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R(n) (c->r[n])
static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }

/* ---- handles of host pointers ---- */

#define HANDLE_BASE 0x7f000000u
enum { MAX_HANDLES = 4096 };
static struct {
    pthread_mutex_t lock;
    void* host[MAX_HANDLES];
    uint32_t count;
} hd = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint32_t handle_of(void* host) {
    if (!host) return 0;
    pthread_mutex_lock(&hd.lock);
    uint32_t i = 0;
    while (i < hd.count && hd.host[i] != host) ++i;
    if (i == hd.count && hd.count < MAX_HANDLES) hd.host[hd.count++] = host;
    pthread_mutex_unlock(&hd.lock);
    return i < MAX_HANDLES ? HANDLE_BASE + 8 * i : 0;
}
static void* host_of(uint32_t handle) {
    if (handle < HANDLE_BASE) return NULL;
    uint32_t i = (handle - HANDLE_BASE) / 8;
    pthread_mutex_lock(&hd.lock);
    void* host = i < hd.count ? hd.host[i] : NULL;
    pthread_mutex_unlock(&hd.lock);
    return host;
}

/* a host string's guest copy, made once for each address (glGetString's and eglQueryString's, which stay) */
static uint32_t string_of(const char* host) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const char* hosts[256];
    static uint32_t guests[256];
    static unsigned count;
    if (!host) return 0;
    pthread_mutex_lock(&lock);
    uint32_t g = 0;
    for (unsigned i = 0; i < count && !g; ++i)
        if (hosts[i] == host) g = guests[i];
    if (!g) {
        g = arm32_strdup(host);
        if (count < 256) {
            hosts[count] = host;
            guests[count++] = g;
        }
    }
    pthread_mutex_unlock(&lock);
    return g;
}

/* ---- the call of a 64-bit handler ---- */

/* A call's kinds, one character for each argument, then ':' and the result's: i an int, u an unsigned int, l a 64-bit
   one (at an even word, as AAPCS gives it), f a float (in a core register, softfp), p a pointer, h a handle, o open's
   flags (ARM's numbers, made AArch64's), a GL_ARRAY_BUFFER pointer (an offset while a buffer is bound there) and e a
   GL_ELEMENT_ARRAY_BUFFER one. Results: v none, i an int, l a 64-bit one, h a handle, s a string, P a pointer into
   the guest's window (another is 0). The handler's errno becomes the guest's, as the 64-bit bridge's exports set it */
typedef struct {
    const char* name;
    const char* kinds;
    const HleSymbol* fn; /* hle_find's, at the first call */
    bool looked;
} Shim;

static _Thread_local CPU* t_cpu;
static CPU* cpu64(void) {
    if (!t_cpu && !(t_cpu = calloc(1, sizeof *t_cpu))) abort();
    return t_cpu;
}

/* a 64-bit handler called with x0.. and v0.. as given, the result in x0 */
static uint64_t call64(const HleSymbol* fn, const uint64_t* x, unsigned nx, const uint32_t* v, unsigned nv) {
    CPU* c = cpu64();
    uint64_t stack[8] = {0};
    c->x[31] = (uint64_t)(uintptr_t)stack;
    for (unsigned i = 0; i < nx; ++i) {
        if (i < 8) c->x[i] = x[i];
        else if (i - 8 < 8) stack[i - 8] = x[i];
    }
    for (unsigned i = 0; i < nv && i < 8; ++i) {
        memset(&c->v[i], 0, sizeof c->v[i]);
        c->v[i].s[0] = v[i];
    }
    fn->handler(c, fn->data);
    return c->x[0];
}

static const HleSymbol* find(Shim* s) {
    if (!__atomic_load_n(&s->looked, __ATOMIC_ACQUIRE)) {
        s->fn = hle_find(s->name);
        __atomic_store_n(&s->looked, true, __ATOMIC_RELEASE);
    }
    return s->fn && s->fn->handler ? s->fn : NULL;
}

/* the GL buffer bound at a binding point (GL_ARRAY_BUFFER_BINDING, GL_ELEMENT_ARRAY_BUFFER_BINDING) of the current
   context */
static int32_t gl_binding(uint32_t pname) {
    static Shim get = {"glGetIntegerv", "up:v", NULL, false};
    const HleSymbol* fn = find(&get);
    int32_t value = 0;
    uint64_t x[2] = {pname, (uint64_t)(uintptr_t)&value};
    if (fn) call64(fn, x, 2, NULL, 0);
    return value;
}

static void shim(SnxArmCPU* c, void* data) {
    Shim* s = data;
    const HleSymbol* fn = find(s);
    if (!fn) { /* a state that has no effect here, or a call the HLE lacks */
        R(0) = 0;
        return;
    }
    uint64_t x[16];
    uint32_t v[8];
    unsigned nx = 0, nv = 0, w = 0;
    const char* k = s->kinds;
    for (; *k && *k != ':' && nx < 16; ++k) {
        if (*k == 'l') {
            w = (w + 1) & ~1u;
            uint32_t lo = arm32_word(c, w++);
            x[nx++] = lo | (uint64_t)arm32_word(c, w++) << 32;
            continue;
        }
        uint32_t word = arm32_word(c, w++);
        switch (*k) {
        case 'o': {
            /* ARM's O_DIRECTORY, O_NOFOLLOW, O_DIRECT and O_LARGEFILE are AArch64's O_DIRECT, O_LARGEFILE,
               O_DIRECTORY and O_NOFOLLOW */
            uint32_t f = word & ~0740000u;
            if (word & 040000) f |= 0200000;
            if (word & 0100000) f |= 0400000;
            if (word & 0200000) f |= 040000;
            if (word & 0400000) f |= 0100000;
            x[nx++] = f;
            break;
        }
        case 'f': v[nv < 8 ? nv++ : 7] = word; break;
        case 'i': x[nx++] = (uint64_t)(int64_t)(int32_t)word; break;
        case 'p': x[nx++] = word ? (uint64_t)(uintptr_t)(arm32_mem + word) : 0; break;
        case 'h': x[nx++] = (uint64_t)(uintptr_t)host_of(word); break;
        case 'a':
        case 'e':
            x[nx++] = word && !gl_binding(*k == 'a' ? 0x8894 : 0x8895) ? (uint64_t)(uintptr_t)(arm32_mem + word) : word;
            break;
        default: x[nx++] = word; break;
        }
    }
    hle_errno_begin();
    *hle_errno_location() = 0;
    uint64_t r = call64(fn, x, nx, v, nv);
    hle_errno_end();
    if (*hle_errno_location()) arm32_set_errno_linux(*hle_errno_location());
    char result = *k == ':' ? k[1] : 'v';
    if (result == 'h') {
        R(0) = handle_of((void*)(uintptr_t)r);
    } else if (result == 's') {
        R(0) = string_of((const char*)(uintptr_t)r);
    } else if (result == 'P') {
        uint8_t* b = (uint8_t*)(uintptr_t)r;
        R(0) = b >= arm32_mem && b < arm32_mem + (UINT64_C(1) << 32) ? arm32_guest(b) : 0;
    } else {
        R(0) = (uint32_t)r;
        if (result == 'l') R(1) = (uint32_t)(r >> 32);
    }
}

uint64_t arm32_call64(const char* name, const uint64_t* x, unsigned count, bool* found) {
    const HleSymbol* fn = hle_find(name);
    if (found) *found = fn && fn->handler;
    if (!fn || !fn->handler) return 0;
    hle_errno_begin();
    *hle_errno_location() = 0;
    uint64_t r = call64(fn, x, count, NULL, 0);
    hle_errno_end();
    if (*hle_errno_location()) arm32_set_errno_linux(*hle_errno_location());
    return r;
}

/* ---- the calls that pointers in memory need ---- */

static Shim s_shader_source = {"glShaderSource", "", NULL, false};
/* glShaderSource(shader, count, strings, lengths): the guest's array of string pointers as the host's */
static void gl_shader_source(SnxArmCPU* c, void* data) {
    (void)data;
    const HleSymbol* fn = find(&s_shader_source);
    uint32_t count = R(1);
    const char** strings = count && count < (1u << 20) ? malloc(count * sizeof *strings) : NULL;
    if (!fn || (count && !strings)) {
        free(strings);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) strings[i] = (const char*)arm32_host(rd32(R(2) + 4 * i));
    uint64_t x[4] = {R(0), count, (uint64_t)(uintptr_t)strings, R(3) ? (uint64_t)(uintptr_t)arm32_host(R(3)) : 0};
    call64(fn, x, 4, NULL, 0);
    free(strings);
}
static Shim s_attrib_pointerv = {"glGetVertexAttribPointerv", "", NULL, false};
/* glGetVertexAttribPointerv(index, pname, pointer): a host pointer back as the guest's, an offset as it is */
static void gl_get_vertex_attrib_pointerv(SnxArmCPU* c, void* data) {
    (void)data;
    const HleSymbol* fn = find(&s_attrib_pointerv);
    void* p = NULL;
    uint64_t x[3] = {R(0), R(1), (uint64_t)(uintptr_t)&p};
    if (fn) call64(fn, x, 3, NULL, 0);
    uint8_t* b = p;
    wr32(R(2), b >= arm32_mem && b < arm32_mem + (UINT64_C(1) << 32) ? arm32_guest(b) : (uint32_t)(uintptr_t)p);
}
static Shim s_choose_config = {"eglChooseConfig", "", NULL, false};
static Shim s_get_configs = {"eglGetConfigs", "", NULL, false};
/* eglChooseConfig(display, attributes, configs, size, count) and eglGetConfigs(display, configs, size, count): the
   host's configs as handles */
static void egl_configs(SnxArmCPU* c, void* data) {
    bool choose = data != NULL;
    const HleSymbol* fn = find(choose ? &s_choose_config : &s_get_configs);
    unsigned at = choose ? 2 : 1;
    uint32_t configs = arm32_word(c, at), size = arm32_word(c, at + 1), count = arm32_word(c, at + 2);
    void** host = configs && size && size < 4096 ? calloc(size, sizeof *host) : NULL;
    uint64_t x[5];
    unsigned n = 0;
    x[n++] = (uint64_t)(uintptr_t)host_of(R(0));
    if (choose) x[n++] = R(1) ? (uint64_t)(uintptr_t)arm32_host(R(1)) : 0;
    x[n++] = (uint64_t)(uintptr_t)host;
    x[n++] = host ? size : 0;
    x[n++] = count ? (uint64_t)(uintptr_t)arm32_host(count) : 0;
    uint32_t ok = fn ? (uint32_t)call64(fn, x, n, NULL, 0) : 0;
    if (ok && host && count) {
        uint32_t got = rd32(count);
        for (uint32_t i = 0; i < got && i < size; ++i) wr32(configs + 4 * i, handle_of(host[i]));
    }
    free(host);
    R(0) = ok;
}
/* eglGetProcAddress(name): the 32-bit bridge's own function of the name, 0 for one it lacks */
static void egl_get_proc_address(SnxArmCPU* c, void* data) {
    (void)data;
    const char* name = (const char*)arm32_host(R(0));
    R(0) = name && (!strncmp(name, "gl", 2) || !strncmp(name, "egl", 3)) ? arm32_hle_symbol(name) : 0;
}

/* ANativeWindow_fromSurface(env, surface): the window as a handle */
static void window_from_surface(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = handle_of(hle_android_window_from_surface(arm32_jni_env(), arm32_jni_ref(R(1))));
}

/* ---- libandroid's own ---- */

/* ALooper: a condition that ALooper_wake signals. No fd or sensor queue gives it events, so a poll waits for its
   timeout or a wake */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool woken;
} Looper;
static _Thread_local uint32_t t_looper;
static Looper* looper_of(uint32_t g) { return g ? *(Looper**)(void*)(arm32_mem + g) : NULL; }
static void looper_prepare(SnxArmCPU* c, void* data) {
    (void)data;
    if (!t_looper) {
        Looper* l = calloc(1, sizeof *l);
        uint32_t g = l ? arm32_malloc(sizeof l) : 0;
        if (g) {
            pthread_mutex_init(&l->lock, NULL);
            pthread_cond_init(&l->cond, NULL);
            memcpy(arm32_mem + g, &l, sizeof l);
            t_looper = g;
        }
    }
    R(0) = t_looper;
}
static void looper_for_thread(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = t_looper;
}
/* ---- AInputQueue: NativeActivity's input events, which the framework sends (NativeBridge.inputQueue*) ---- */

/* An event is a guest block: type, device, source, action, flags, meta state, edge flags, button state, down time and
   event time (nanoseconds), x and y precision, pointer count, key code, scan code, repeat count, then for each pointer
   its id, tool type, x, y, pressure, size, touch major and minor, tool major and minor, orientation */
enum { EV_TYPE = 0, EV_DEVICE = 4, EV_SOURCE = 8, EV_ACTION = 12, EV_FLAGS = 16, EV_META = 20, EV_EDGES = 24,
       EV_BUTTONS = 28, EV_DOWN = 32, EV_TIME = 40, EV_XPREC = 48, EV_YPREC = 52, EV_POINTERS = 56, EV_KEY = 60,
       EV_SCAN = 64, EV_REPEAT = 68, EV_POINTER = 72, POINTER_SIZE = 44, MAX_POINTERS = 10,
       EVENT_SIZE = EV_POINTER + MAX_POINTERS * POINTER_SIZE };
enum { QUEUE_EVENTS = 256 };
typedef struct InputQueue {
    uint32_t handle;
    uint32_t events[QUEUE_EVENTS];
    unsigned head, count;
    uint32_t looper, callback, data; /* attachLooper's */
    int32_t ident;
    struct InputQueue* next;
} InputQueue;
static InputQueue* queues;
static pthread_mutex_t queues_lock = PTHREAD_MUTEX_INITIALIZER;
static InputQueue* queue_of(uint32_t handle) { /* under queues_lock */
    for (InputQueue* q = queues; q; q = q->next)
        if (q->handle == handle) return q;
    return NULL;
}

uint32_t arm32_input_queue_new(void) {
    InputQueue* q = calloc(1, sizeof *q);
    uint32_t g = q ? arm32_malloc(16) : 0;
    if (!g) {
        free(q);
        return 0;
    }
    q->handle = g;
    pthread_mutex_lock(&queues_lock);
    q->next = queues;
    queues = q;
    pthread_mutex_unlock(&queues_lock);
    return g;
}
static void wake(uint32_t looper);
/* an event, made by fill, onto the end of the queue: the looper it is attached to woken */
static uint32_t new_event(void) {
    uint32_t e = arm32_malloc(EVENT_SIZE);
    if (e) memset(arm32_mem + e, 0, EVENT_SIZE);
    return e;
}
static void push(uint32_t queue, uint32_t e) {
    pthread_mutex_lock(&queues_lock);
    InputQueue* q = queue_of(queue);
    uint32_t looper = 0;
    if (q && q->count < QUEUE_EVENTS) {
        q->events[(q->head + q->count++) % QUEUE_EVENTS] = e;
        looper = q->looper;
        e = 0;
    }
    pthread_mutex_unlock(&queues_lock);
    arm32_free(e); /* a full or gone queue drops it */
    if (looper) wake(looper);
}
static void put64(uint32_t a, int64_t v) { memcpy(arm32_mem + a, &v, 8); }
static uint32_t bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
void arm32_input_motion(uint32_t queue, const int32_t* fields, int64_t down, int64_t time, float xprec, float yprec,
                        unsigned count, const int32_t* ids, const int32_t* tools, const float* axes) {
    uint32_t e = new_event();
    if (!e) return;
    wr32(e + EV_TYPE, 2); /* AINPUT_EVENT_TYPE_MOTION */
    for (unsigned i = 0; i < 7; ++i) wr32(e + EV_DEVICE + 4 * i, (uint32_t)fields[i]);
    put64(e + EV_DOWN, down);
    put64(e + EV_TIME, time);
    wr32(e + EV_XPREC, bits(xprec));
    wr32(e + EV_YPREC, bits(yprec));
    if (count > MAX_POINTERS) count = MAX_POINTERS;
    wr32(e + EV_POINTERS, count);
    for (unsigned p = 0; p < count; ++p) {
        uint32_t at = e + EV_POINTER + POINTER_SIZE * p;
        wr32(at, (uint32_t)ids[p]);
        wr32(at + 4, (uint32_t)tools[p]);
        for (unsigned k = 0; k < 9; ++k) wr32(at + 8 + 4 * k, bits(axes[9 * p + k]));
    }
    push(queue, e);
}
void arm32_input_key(uint32_t queue, const int32_t* fields, int64_t down, int64_t time) {
    uint32_t e = new_event();
    if (!e) return;
    wr32(e + EV_TYPE, 1); /* AINPUT_EVENT_TYPE_KEY */
    /* device, source, action, flags, key code, scan code, meta state, repeat count */
    wr32(e + EV_DEVICE, (uint32_t)fields[0]);
    wr32(e + EV_SOURCE, (uint32_t)fields[1]);
    wr32(e + EV_ACTION, (uint32_t)fields[2]);
    wr32(e + EV_FLAGS, (uint32_t)fields[3]);
    wr32(e + EV_KEY, (uint32_t)fields[4]);
    wr32(e + EV_SCAN, (uint32_t)fields[5]);
    wr32(e + EV_META, (uint32_t)fields[6]);
    wr32(e + EV_REPEAT, (uint32_t)fields[7]);
    put64(e + EV_DOWN, down);
    put64(e + EV_TIME, time);
    push(queue, e);
}

/* AInputQueue_attachLooper(queue, looper, ident, callback, data) */
static void queue_attach(SnxArmCPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&queues_lock);
    InputQueue* q = queue_of(R(0));
    if (q) {
        q->looper = R(1);
        q->ident = (int32_t)R(2);
        q->callback = R(3);
        q->data = arm32_word(c, 4);
    }
    bool pending = q && q->count;
    pthread_mutex_unlock(&queues_lock);
    if (pending) wake(R(1));
}
static void queue_detach(SnxArmCPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&queues_lock);
    InputQueue* q = queue_of(R(0));
    if (q) q->looper = 0;
    pthread_mutex_unlock(&queues_lock);
}
/* AInputQueue_getEvent(queue, &event): 0 and the oldest event, or -1 with none */
static void queue_get(SnxArmCPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&queues_lock);
    InputQueue* q = queue_of(R(0));
    uint32_t e = 0;
    if (q && q->count) {
        e = q->events[q->head];
        q->head = (q->head + 1) % QUEUE_EVENTS;
        --q->count;
    }
    pthread_mutex_unlock(&queues_lock);
    if (e && R(1)) wr32(R(1), e);
    R(0) = e ? 0 : UINT32_MAX;
}
static void queue_has(SnxArmCPU* c, void* data) {
    (void)data;
    pthread_mutex_lock(&queues_lock);
    InputQueue* q = queue_of(R(0));
    R(0) = q ? (q->count ? 1 : 0) : UINT32_MAX;
    pthread_mutex_unlock(&queues_lock);
}
/* AInputQueue_finishEvent(queue, event, handled): the event's block freed */
static void queue_finish(SnxArmCPU* c, void* data) {
    (void)data;
    arm32_free(R(1));
}

/* the event getters: a word of the event (data: its offset), a 64-bit time, a float, or a pointer's (r1: the pointer's
   index) */
static void event_word(SnxArmCPU* c, void* data) { R(0) = rd32(R(0) + (uint32_t)(uintptr_t)data); }
static void event_time(SnxArmCPU* c, void* data) {
    uint32_t at = R(0) + (uint32_t)(uintptr_t)data;
    R(0) = rd32(at);
    R(1) = rd32(at + 4);
}
static uint32_t pointer_at(SnxArmCPU* c, unsigned field) {
    uint32_t n = rd32(R(0) + EV_POINTERS);
    uint32_t p = R(1) < n ? R(1) : 0;
    return R(0) + EV_POINTER + POINTER_SIZE * p + field;
}
static void pointer_word(SnxArmCPU* c, void* data) { R(0) = rd32(pointer_at(c, (unsigned)(uintptr_t)data)); }
/* AMotionEvent_getAxisValue(event, axis, pointer): X, Y, PRESSURE, SIZE, TOUCH_MAJOR, TOUCH_MINOR, TOOL_MAJOR,
   TOOL_MINOR, ORIENTATION are axes 0 to 8 */
static void pointer_axis(SnxArmCPU* c, void* data) {
    (void)data;
    uint32_t axis = R(1);
    R(1) = R(2);
    R(0) = axis < 9 ? rd32(pointer_at(c, 8 + 4 * axis)) : 0;
}
/* AMotionEvent_getHistorical*(event, pointer, history): no history (getHistorySize is 0), the current values */
static void pointer_historical(SnxArmCPU* c, void* data) { R(0) = rd32(pointer_at(c, (unsigned)(uintptr_t)data)); }
#define EV(name, offset) {name, event_word, (void*)(uintptr_t)(offset)}
#define EV_T(name, offset) {name, event_time, (void*)(uintptr_t)(offset)}
#define PT(name, offset) {name, pointer_word, (void*)(uintptr_t)(8 + (offset))}

/* ALooper_pollAll / pollOnce(timeout, fd, events, data): an attached input queue with events runs its callback, or
   gives its ident; else ALOOPER_POLL_WAKE (-1) when woken, ALOOPER_POLL_TIMEOUT (-3) after the timeout (none for
   -1) */
static void looper_poll(SnxArmCPU* c, void* data) {
    (void)data;
    Looper* l = looper_of(t_looper);
    int32_t timeout = (int32_t)R(0);
    uint32_t out_fd = R(1), out_events = R(2), out_data = R(3);
    if (!l) {
        R(0) = (uint32_t)-4; /* ALOOPER_POLL_ERROR */
        return;
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        /* an attached queue with events */
        pthread_mutex_lock(&queues_lock);
        InputQueue* ready = NULL;
        for (InputQueue* q = queues; q && !ready; q = q->next)
            if (q->looper == t_looper && q->count) ready = q;
        uint32_t callback = ready ? ready->callback : 0, qdata = ready ? ready->data : 0;
        int32_t ident = ready ? ready->ident : 0;
        pthread_mutex_unlock(&queues_lock);
        if (ready && callback) {
            uint32_t args[3] = {UINT32_MAX, 1 /* ALOOPER_EVENT_INPUT */, qdata};
            arm32_call(callback, args, 3, NULL);
        } else if (ready) {
            if (out_fd) wr32(out_fd, UINT32_MAX);
            if (out_events) wr32(out_events, 1);
            if (out_data) wr32(out_data, qdata);
            R(0) = (uint32_t)ident;
            return;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t spent = (int64_t)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        pthread_mutex_lock(&l->lock);
        bool woken = l->woken;
        l->woken = false;
        if (!woken && timeout != 0 && (timeout < 0 || spent < timeout) && !ready) {
            /* in slices, so that a signal sent to the thread runs its handler meanwhile */
            int64_t step = timeout < 0 || timeout - spent > 10 ? 10 : timeout - spent;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += (long)(step * 1000000);
            if (until.tv_nsec >= 1000000000) until.tv_sec += 1, until.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&l->cond, &l->lock, &until);
            woken = l->woken;
            l->woken = false;
        }
        pthread_mutex_unlock(&l->lock);
        if (woken) {
            /* a wake that an event made goes to the event first */
            pthread_mutex_lock(&queues_lock);
            bool events = false;
            for (InputQueue* q = queues; q && !events; q = q->next) events = q->looper == t_looper && q->count;
            pthread_mutex_unlock(&queues_lock);
            if (events) continue;
            R(0) = (uint32_t)-1;
            return;
        }
        if (__atomic_load_n(&arm32_pending_signals, __ATOMIC_RELAXED)) arm32_signals_run(c);
        clock_gettime(CLOCK_MONOTONIC, &now);
        spent = (int64_t)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (timeout == 0 || (timeout > 0 && spent >= timeout)) {
            R(0) = (uint32_t)-3;
            return;
        }
    }
}
static void wake(uint32_t looper) {
    Looper* l = looper_of(looper);
    if (!l) return;
    pthread_mutex_lock(&l->lock);
    l->woken = true;
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->lock);
}
static void looper_wake(SnxArmCPU* c, void* data) {
    (void)data;
    wake(R(0));
}
static void zero(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = 0;
}
static void minus_one(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = UINT32_MAX;
}
static void nothing(SnxArmCPU* c, void* data) { (void)c, (void)data; }
/* AConfiguration: a block the guest may hand back; the values are the defaults (0, "any") */
static void config_new(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = arm32_malloc(64);
    if (R(0)) memset(arm32_mem + R(0), 0, 64);
}
static void config_delete(SnxArmCPU* c, void* data) {
    (void)data;
    arm32_free(R(0));
}
/* ASensorManager: one, with no sensors */
static uint32_t sensor_manager;
static void sensor_manager_instance(SnxArmCPU* c, void* data) {
    (void)data;
    if (!sensor_manager) sensor_manager = arm32_malloc(16);
    R(0) = sensor_manager;
}
static void sensor_list(SnxArmCPU* c, void* data) {
    (void)data;
    if (R(1)) wr32(R(1), 0);
    R(0) = 0;
}
static void sensor_queue(SnxArmCPU* c, void* data) {
    (void)data;
    R(0) = arm32_malloc(16);
}
static void sensor_queue_destroy(SnxArmCPU* c, void* data) {
    (void)data;
    arm32_free(R(1));
    R(0) = 0;
}

/* ---- the table ---- */

#define S(name, kinds) {name, shim, &(Shim){name, kinds, NULL, false}}
#define OWN(name, fn, data) {name, fn, data}
const Arm32Symbol arm32_shim_symbols[] = {
    /* EGL */
    S("eglGetDisplay", "u:h"),
    S("eglInitialize", "hpp:i"),
    S("eglTerminate", "h:i"),
    S("eglQueryString", "hi:s"),
    S("eglGetError", ":i"),
    OWN("eglChooseConfig", egl_configs, (void*)1),
    OWN("eglGetConfigs", egl_configs, NULL),
    S("eglGetConfigAttrib", "hhip:i"),
    S("eglCreateContext", "hhhp:h"),
    S("eglDestroyContext", "hh:i"),
    S("eglQueryContext", "hhip:i"),
    S("eglCreateWindowSurface", "hhhp:h"),
    S("eglCreatePbufferSurface", "hhp:h"),
    S("eglDestroySurface", "hh:i"),
    S("eglQuerySurface", "hhip:i"),
    S("eglMakeCurrent", "hhhh:i"),
    S("eglGetCurrentContext", ":h"),
    S("eglGetCurrentDisplay", ":h"),
    S("eglGetCurrentSurface", "i:h"),
    S("eglSwapBuffers", "hh:i"),
    S("eglSwapInterval", "hi:i"),
    S("eglBindAPI", "u:i"),
    S("eglWaitGL", ":i"),
    S("eglWaitClient", ":i"),
    S("eglReleaseThread", ":i"),
    OWN("eglGetProcAddress", egl_get_proc_address, NULL),
    /* ANativeWindow */
    OWN("ANativeWindow_fromSurface", window_from_surface, NULL),
    S("ANativeWindow_acquire", "h:v"),
    S("ANativeWindow_release", "h:v"),
    S("ANativeWindow_getWidth", "h:i"),
    S("ANativeWindow_getHeight", "h:i"),
    S("ANativeWindow_getFormat", "h:i"),
    S("ANativeWindow_setBuffersGeometry", "hiii:i"),
    /* GLES 1.1 and 2.0: a name of both calls the current context's */
    S("glActiveTexture", "u:v"),
    S("glAlphaFunc", "uf:v"),
    S("glAttachShader", "uu:v"),
    S("glBindAttribLocation", "uup:v"),
    S("glBindBuffer", "uu:v"),
    S("glBindFramebuffer", "uu:v"),
    S("glBindRenderbuffer", "uu:v"),
    S("glBindTexture", "uu:v"),
    S("glBlendColor", "ffff:v"),
    S("glBlendEquation", "u:v"),
    S("glBlendEquationSeparate", "uu:v"),
    S("glBlendFunc", "uu:v"),
    S("glBlendFuncSeparate", "uuuu:v"),
    S("glBufferData", "uipu:v"),
    S("glBufferSubData", "uiip:v"),
    S("glCheckFramebufferStatus", "u:i"),
    S("glClear", "u:v"),
    S("glClearColor", "ffff:v"),
    S("glClearDepthf", "f:v"),
    S("glClearStencil", "i:v"),
    S("glClientActiveTexture", "u:v"),
    S("glColor4f", "ffff:v"),
    S("glColor4ub", "uuuu:v"),
    S("glColorMask", "uuuu:v"),
    S("glColorPointer", "iuia:v"),
    S("glCompileShader", "u:v"),
    S("glCompressedTexImage2D", "uiuiiiip:v"),
    S("glCompressedTexSubImage2D", "uiiiiiuip:v"),
    S("glCopyTexImage2D", "uiuiiiii:v"),
    S("glCopyTexSubImage2D", "uiiiiiii:v"),
    S("glCreateProgram", ":i"),
    S("glCreateShader", "u:i"),
    S("glCullFace", "u:v"),
    S("glDeleteBuffers", "ip:v"),
    S("glDeleteFramebuffers", "ip:v"),
    S("glDeleteProgram", "u:v"),
    S("glDeleteRenderbuffers", "ip:v"),
    S("glDeleteShader", "u:v"),
    S("glDeleteTextures", "ip:v"),
    S("glDepthFunc", "u:v"),
    S("glDepthMask", "u:v"),
    S("glDepthRangef", "ff:v"),
    S("glDetachShader", "uu:v"),
    S("glDisable", "u:v"),
    S("glDisableClientState", "u:v"),
    S("glDisableVertexAttribArray", "u:v"),
    S("glDiscardFramebufferEXT", "uip:v"),
    S("glDrawArrays", "uii:v"),
    S("glDrawElements", "uiue:v"),
    S("glDrawTexiOES", "iiiii:v"),
    S("glEnable", "u:v"),
    S("glEnableClientState", "u:v"),
    S("glEnableVertexAttribArray", "u:v"),
    S("glFinish", ":v"),
    S("glFlush", ":v"),
    S("glFramebufferRenderbuffer", "uuuu:v"),
    S("glFramebufferTexture2D", "uuuui:v"),
    S("glFrontFace", "u:v"),
    S("glGenBuffers", "ip:v"),
    S("glGenFramebuffers", "ip:v"),
    S("glGenRenderbuffers", "ip:v"),
    S("glGenTextures", "ip:v"),
    S("glGenerateMipmap", "u:v"),
    S("glGetActiveAttrib", "uuipppp:v"),
    S("glGetActiveUniform", "uuipppp:v"),
    S("glGetAttachedShaders", "uipp:v"),
    S("glGetAttribLocation", "up:i"),
    S("glGetBooleanv", "up:v"),
    S("glGetBufferParameteriv", "uup:v"),
    S("glGetError", ":i"),
    S("glGetFloatv", "up:v"),
    S("glGetFramebufferAttachmentParameteriv", "uuup:v"),
    S("glGetIntegerv", "up:v"),
    S("glGetProgramInfoLog", "uipp:v"),
    S("glGetProgramiv", "uup:v"),
    S("glGetRenderbufferParameteriv", "uup:v"),
    S("glGetShaderInfoLog", "uipp:v"),
    S("glGetShaderPrecisionFormat", "uupp:v"),
    S("glGetShaderSource", "uipp:v"),
    S("glGetShaderiv", "uup:v"),
    S("glGetString", "u:s"),
    S("glGetTexParameteriv", "uup:v"),
    S("glGetUniformLocation", "up:i"),
    S("glGetUniformfv", "uip:v"),
    S("glGetUniformiv", "uip:v"),
    OWN("glGetVertexAttribPointerv", gl_get_vertex_attrib_pointerv, NULL),
    S("glGetVertexAttribfv", "uup:v"),
    S("glGetVertexAttribiv", "uup:v"),
    S("glHint", "uu:v"),
    S("glIsBuffer", "u:i"),
    S("glIsEnabled", "u:i"),
    S("glIsFramebuffer", "u:i"),
    S("glIsProgram", "u:i"),
    S("glIsRenderbuffer", "u:i"),
    S("glIsShader", "u:i"),
    S("glIsTexture", "u:i"),
    S("glLineWidth", "f:v"),
    S("glLinkProgram", "u:v"),
    S("glLoadIdentity", ":v"),
    S("glLoadMatrixf", "p:v"),
    S("glMatrixMode", "u:v"),
    S("glMultMatrixf", "p:v"),
    S("glOrthof", "ffffff:v"),
    S("glPixelStorei", "ui:v"),
    S("glPolygonOffset", "ff:v"),
    S("glPopMatrix", ":v"),
    S("glPushMatrix", ":v"),
    S("glReadPixels", "iiiiuup:v"),
    S("glReleaseShaderCompiler", ":v"),
    S("glRenderbufferStorage", "uuii:v"),
    S("glRotatef", "ffff:v"),
    S("glSampleCoverage", "fu:v"),
    S("glScalef", "fff:v"),
    S("glScissor", "iiii:v"),
    S("glShadeModel", "u:v"),
    OWN("glShaderSource", gl_shader_source, NULL),
    S("glStencilFunc", "uiu:v"),
    S("glStencilFuncSeparate", "uuiu:v"),
    S("glStencilMask", "u:v"),
    S("glStencilMaskSeparate", "uu:v"),
    S("glStencilOp", "uuu:v"),
    S("glStencilOpSeparate", "uuuu:v"),
    S("glTexCoordPointer", "iuia:v"),
    S("glTexEnvf", "uuf:v"),
    S("glTexEnvi", "uui:v"),
    S("glTexImage2D", "uiiiiiuup:v"),
    S("glTexParameterf", "uuf:v"),
    S("glTexParameterfv", "uup:v"),
    S("glTexParameteri", "uui:v"),
    S("glTexParameteriv", "uup:v"),
    S("glTexSubImage2D", "uiiiiiuup:v"),
    S("glTranslatef", "fff:v"),
    S("glUniform1f", "if:v"),
    S("glUniform1fv", "iip:v"),
    S("glUniform1i", "ii:v"),
    S("glUniform1iv", "iip:v"),
    S("glUniform2f", "iff:v"),
    S("glUniform2fv", "iip:v"),
    S("glUniform2i", "iii:v"),
    S("glUniform2iv", "iip:v"),
    S("glUniform3f", "ifff:v"),
    S("glUniform3fv", "iip:v"),
    S("glUniform3i", "iiii:v"),
    S("glUniform3iv", "iip:v"),
    S("glUniform4f", "iffff:v"),
    S("glUniform4fv", "iip:v"),
    S("glUniform4i", "iiiii:v"),
    S("glUniform4iv", "iip:v"),
    S("glUniformMatrix2fv", "iiup:v"),
    S("glUniformMatrix3fv", "iiup:v"),
    S("glUniformMatrix4fv", "iiup:v"),
    S("glUseProgram", "u:v"),
    S("glValidateProgram", "u:v"),
    S("glVertexAttrib1f", "uf:v"),
    S("glVertexAttrib2f", "uff:v"),
    S("glVertexAttrib3f", "ufff:v"),
    S("glVertexAttrib4f", "uffff:v"),
    S("glVertexAttrib4fv", "up:v"),
    S("glVertexAttribPointer", "uiuuia:v"),
    S("glVertexPointer", "iuia:v"),
    S("glViewport", "iiii:v"),
    /* bionic's that the 64-bit bridge has with the same arguments: files, descriptors, sockets, the process */
    S("open", "pou:i"),
    S("open64", "pou:i"),
    S("openat", "ipou:i"),
    S("creat", "pu:i"),
    S("read", "ipu:i"),
    S("write", "ipu:i"),
    S("close", "i:i"),
    S("lseek", "iii:i"),
    S("lseek64", "ili:l"),
    S("pread", "ipui:i"),
    S("pwrite", "ipui:i"),
    S("pread64", "ipul:i"),
    S("pwrite64", "ipul:i"),
    S("access", "pi:i"),
    S("faccessat", "ipii:i"),
    S("unlink", "p:i"),
    S("unlinkat", "ipi:i"),
    S("rename", "pp:i"),
    S("mkdir", "pu:i"),
    S("rmdir", "p:i"),
    S("chdir", "p:i"),
    S("fchdir", "i:i"),
    S("chmod", "pu:i"),
    S("fchmod", "iu:i"),
    S("chown", "puu:i"),
    S("fsync", "i:i"),
    S("fdatasync", "i:i"),
    S("ftruncate", "ii:i"),
    S("ftruncate64", "il:i"),
    S("truncate", "pi:i"),
    S("readlink", "ppu:i"),
    S("symlink", "pp:i"),
    S("link", "pp:i"),
    S("dup", "i:i"),
    S("dup2", "ii:i"),
    S("pipe", "p:i"),
    S("pipe2", "pi:i"),
    S("isatty", "i:i"),
    S("ioctl", "iup:i"),
    S("flock", "ii:i"),
    S("umask", "u:i"),
    S("mkstemp", "p:i"),
    S("getpid", ":i"),
    S("getppid", ":i"),
    S("getuid", ":i"),
    S("geteuid", ":i"),
    S("getgid", ":i"),
    S("getegid", ":i"),
    S("getpgrp", ":i"),
    S("setsid", ":i"),
    S("getpriority", "ii:i"),
    S("setpriority", "iii:i"),
    S("sched_get_priority_max", "i:i"),
    S("sched_get_priority_min", "i:i"),
    S("sysconf", "i:i"),
    S("uname", "p:i"),
    S("gethostname", "pu:i"),
    S("__system_property_get", "pp:i"),
    S("socket", "iii:i"),
    S("socketpair", "iiip:i"),
    S("bind", "ipu:i"),
    S("connect", "ipu:i"),
    S("listen", "ii:i"),
    S("accept", "ipp:i"),
    S("shutdown", "ii:i"),
    S("send", "ipui:i"),
    S("recv", "ipui:i"),
    S("sendto", "ipuipu:i"),
    S("recvfrom", "ipuipp:i"),
    S("getsockname", "ipp:i"),
    S("getpeername", "ipp:i"),
    S("epoll_create", "i:i"),
    S("epoll_create1", "i:i"),
    S("epoll_ctl", "iiip:i"),
    S("eventfd", "ui:i"),
    /* libandroid */
    OWN("ALooper_prepare", looper_prepare, NULL),
    OWN("ALooper_forThread", looper_for_thread, NULL),
    OWN("ALooper_pollAll", looper_poll, NULL),
    OWN("ALooper_pollOnce", looper_poll, NULL),
    OWN("ALooper_wake", looper_wake, NULL),
    OWN("ALooper_acquire", nothing, NULL),
    OWN("ALooper_release", nothing, NULL),
    OWN("AInputQueue_attachLooper", queue_attach, NULL),
    OWN("AInputQueue_detachLooper", queue_detach, NULL),
    OWN("AInputQueue_getEvent", queue_get, NULL),
    OWN("AInputQueue_hasEvents", queue_has, NULL),
    OWN("AInputQueue_preDispatchEvent", zero, NULL),
    OWN("AInputQueue_finishEvent", queue_finish, NULL),
    EV("AInputEvent_getType", EV_TYPE),
    EV("AInputEvent_getDeviceId", EV_DEVICE),
    EV("AInputEvent_getSource", EV_SOURCE),
    EV("AKeyEvent_getAction", EV_ACTION),
    EV("AKeyEvent_getFlags", EV_FLAGS),
    EV("AKeyEvent_getKeyCode", EV_KEY),
    EV("AKeyEvent_getScanCode", EV_SCAN),
    EV("AKeyEvent_getMetaState", EV_META),
    EV("AKeyEvent_getRepeatCount", EV_REPEAT),
    EV_T("AKeyEvent_getDownTime", EV_DOWN),
    EV_T("AKeyEvent_getEventTime", EV_TIME),
    EV("AMotionEvent_getAction", EV_ACTION),
    EV("AMotionEvent_getFlags", EV_FLAGS),
    EV("AMotionEvent_getMetaState", EV_META),
    EV("AMotionEvent_getButtonState", EV_BUTTONS),
    EV("AMotionEvent_getEdgeFlags", EV_EDGES),
    EV_T("AMotionEvent_getDownTime", EV_DOWN),
    EV_T("AMotionEvent_getEventTime", EV_TIME),
    EV("AMotionEvent_getXPrecision", EV_XPREC),
    EV("AMotionEvent_getYPrecision", EV_YPREC),
    EV("AMotionEvent_getPointerCount", EV_POINTERS),
    OWN("AMotionEvent_getHistorySize", zero, NULL),
    EV_T("AMotionEvent_getHistoricalEventTime", EV_TIME),
    OWN("AMotionEvent_getXOffset", zero, NULL),
    OWN("AMotionEvent_getYOffset", zero, NULL),
    {"AMotionEvent_getPointerId", pointer_word, (void*)0},
    {"AMotionEvent_getToolType", pointer_word, (void*)4},
    PT("AMotionEvent_getX", 0),
    PT("AMotionEvent_getY", 4),
    PT("AMotionEvent_getRawX", 0),
    PT("AMotionEvent_getRawY", 4),
    PT("AMotionEvent_getPressure", 8),
    PT("AMotionEvent_getSize", 12),
    PT("AMotionEvent_getTouchMajor", 16),
    PT("AMotionEvent_getTouchMinor", 20),
    PT("AMotionEvent_getToolMajor", 24),
    PT("AMotionEvent_getToolMinor", 28),
    PT("AMotionEvent_getOrientation", 32),
    OWN("AMotionEvent_getAxisValue", pointer_axis, NULL),
    {"AMotionEvent_getHistoricalX", pointer_historical, (void*)8},
    {"AMotionEvent_getHistoricalY", pointer_historical, (void*)12},
    {"AMotionEvent_getHistoricalPressure", pointer_historical, (void*)16},
    {"AMotionEvent_getHistoricalSize", pointer_historical, (void*)20},
    {"AMotionEvent_getHistoricalRawX", pointer_historical, (void*)8},
    {"AMotionEvent_getHistoricalRawY", pointer_historical, (void*)12},
    {"AMotionEvent_getHistoricalTouchMajor", pointer_historical, (void*)24},
    {"AMotionEvent_getHistoricalTouchMinor", pointer_historical, (void*)28},
    {"AMotionEvent_getHistoricalToolMajor", pointer_historical, (void*)32},
    {"AMotionEvent_getHistoricalToolMinor", pointer_historical, (void*)36},
    {"AMotionEvent_getHistoricalOrientation", pointer_historical, (void*)40},
    {"AMotionEvent_getHistoricalAxisValue", pointer_axis, NULL},
    OWN("AConfiguration_new", config_new, NULL),
    OWN("AConfiguration_delete", config_delete, NULL),
    OWN("AConfiguration_fromAssetManager", nothing, NULL),
    OWN("ASensorManager_getInstance", sensor_manager_instance, NULL),
    OWN("ASensorManager_getSensorList", sensor_list, NULL),
    OWN("ASensorManager_getDefaultSensor", zero, NULL),
    OWN("ASensorManager_createEventQueue", sensor_queue, NULL),
    OWN("ASensorManager_destroyEventQueue", sensor_queue_destroy, NULL),
    OWN("ASensorEventQueue_enableSensor", minus_one, NULL),
    OWN("ASensorEventQueue_disableSensor", zero, NULL),
    OWN("ASensorEventQueue_setEventRate", zero, NULL),
    OWN("ASensorEventQueue_hasEvents", zero, NULL),
    OWN("ASensorEventQueue_getEvents", zero, NULL),
    {"", NULL, NULL},
};

void arm32_shim_init(void) {}
