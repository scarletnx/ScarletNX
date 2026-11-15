/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An eventfd as a socket of the stack, so it can be polled with the others: Horizon's bsd has EventFd (7.0.0+),
   which Eden only stubs, and the SDK uses to interrupt a Poll from another thread. Linux's semantics: reads take
   the counter (or 1 of it, as a semaphore) and block while it is 0, writes add to it. A notifier is readable while
   the counter isn't 0. */
#include "common/network/net_internal.h"
#include <stdlib.h>
#include <string.h>

#define SNX_EVENT_COUNT_MAX 0xFFFFFFFFFFFFFFFEull
typedef struct Event {
    SnxLock lock;
    uint64_t count;
    int semaphore;
    SnxHostFd notify[2];
} Event;

static void event_destroy(SnxSocket* s) {
    Event* e = s->impl;
    snx_net_close_fd(e->notify[0]);
    snx_net_close_fd(e->notify[1]);
    snx_lock_destroy(&e->lock);
    free(e);
}
static int64_t event_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags) {
    Event* e = s->impl;
    if (from) memset(from, 0, sizeof *from);
    if (out_flags) *out_flags = 0;
    if (size < 8) return -SNX_EINVAL;
    for (;;) {
        snx_lock(&e->lock);
        if (e->count) {
            uint64_t v = e->semaphore ? 1 : e->count;
            e->count -= v;
            if (!e->count) snx_notifier_drain(e->notify[0]);
            snx_unlock(&e->lock);
            uint8_t le[8];
            for (int i = 0; i < 8; i++) le[i] = (uint8_t)(v >> (i * 8));
            memcpy(data, le, 8);
            return 8;
        }
        snx_unlock(&e->lock);
        if (s->nonblocking || (flags & SNX_MSG_DONTWAIT)) return -SNX_EAGAIN;
        int r = snx_net_wait(e->notify[0], POLLIN, 0);
        if (r < 0) return r;
    }
}
static int64_t event_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to) {
    Event* e = s->impl;
    (void)flags, (void)to;
    if (size < 8) return -SNX_EINVAL;
    const uint8_t* p = data;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    if (v == UINT64_MAX) return -SNX_EINVAL;
    snx_lock(&e->lock);
    if (v > SNX_EVENT_COUNT_MAX - e->count) {
        snx_unlock(&e->lock);
        return -SNX_EAGAIN; /* Linux would block until a read makes room. Nothing written waits that long */
    }
    int was_zero = !e->count;
    e->count += v;
    if (was_zero && e->count) snx_notifier_signal(e->notify[1]);
    snx_unlock(&e->lock);
    return 8;
}
static int event_readable_bytes(SnxSocket* s, int* bytes) {
    Event* e = s->impl;
    snx_lock(&e->lock);
    *bytes = e->count ? 8 : 0;
    snx_unlock(&e->lock);
    return 0;
}
static int event_setsockopt(SnxSocket* s, int level, int name, const void* v, size_t size) {
    return snx_net_sockopt_kept(level, name) ? snx_net_host_setsockopt(s, level, name, v, size) : -SNX_ENOTSOCK;
}
static int event_getsockopt(SnxSocket* s, int level, int name, void* v, size_t* size) {
    return snx_net_sockopt_kept(level, name) ? snx_net_host_getsockopt(s, level, name, v, size) : -SNX_ENOTSOCK;
}
static uint16_t event_poll_ready(SnxSocket* s, uint16_t events, short* host_events) {
    Event* e = s->impl;
    *host_events = (events & (SNX_POLLIN | SNX_POLLRDNORM)) ? POLLIN : 0;
    snx_lock(&e->lock);
    int room = e->count < SNX_EVENT_COUNT_MAX;
    snx_unlock(&e->lock);
    return room ? events & (SNX_POLLOUT | SNX_POLLWRNORM) : 0;
}
static const SnxSocketOps g_event_ops = {.destroy = event_destroy,
                                         .send = event_send,
                                         .recv = event_recv,
                                         .setsockopt = event_setsockopt,
                                         .getsockopt = event_getsockopt,
                                         .readable_bytes = event_readable_bytes,
                                         .poll_ready = event_poll_ready};

int snx_net_event_open(SnxSocket** out, uint64_t initial, int semaphore) {
    if (initial > SNX_EVENT_COUNT_MAX) return -SNX_EINVAL;
    Event* e = calloc(1, sizeof *e);
    if (!e) return -SNX_ENOMEM;
    int r = snx_notifier_open(e->notify);
    if (r) {
        free(e);
        return r;
    }
    snx_lock_init(&e->lock);
    e->count = initial;
    e->semaphore = semaphore;
    if (initial) snx_notifier_signal(e->notify[1]);
    SnxSocket* s = snx_net_socket_new(&g_event_ops, e->notify[0], SNX_AF_UNSPEC, 0, 0);
    if (!s) {
        snx_net_close_fd(e->notify[0]);
        snx_net_close_fd(e->notify[1]);
        free(e);
        return -SNX_ENOMEM;
    }
    s->impl = e;
    *out = s;
    return 0;
}
int snx_socket_event_get(SnxSocket* s, uint64_t* count, int* semaphore) {
    if (s->ops != &g_event_ops) return -SNX_EINVAL;
    Event* e = s->impl;
    snx_lock(&e->lock);
    *count = e->count;
    snx_unlock(&e->lock);
    *semaphore = e->semaphore;
    return 0;
}
