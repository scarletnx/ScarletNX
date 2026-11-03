/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ppoll, pselect6 and epoll over every kind of descriptor: sockets and eventfds through the stack's poll, stdio
   through its host descriptor, and the guest's own files, which are always ready, as Linux's regular files are.
   epoll is level-triggered poll over its interest list. Edge-triggered users read until EAGAIN anyway, so EPOLLET
   only costs them a spare wakeup. EPOLLONESHOT disarms an entry once it was reported. The calls' signal masks are
   the thread's while they wait (signals.c's gnu_signal_call_mask). An epoll's lock guards its interest list; a wait
   polls the list as it was when the wait started, so an epoll_ctl of another thread during it is seen at the next
   wait */
#include "gnuabi/network/network.h"
#include "gnuabi/filesystem/internal.h"
#include <stdlib.h>
#include <string.h>

enum {
    EPOLL_CLOEXEC = 0x80000,
    EPOLL_CTL_ADD = 1,
    EPOLL_CTL_DEL = 2,
    EPOLL_CTL_MOD = 3,
    EPOLL_EVENT_SIZE = 16, /* {u32 events, u64 data}: AArch64 aligns data, x86-64 packs it */
    POLL_MAX = 4096,
    SELECT_MAX = 1024, /* FD_SETSIZE */
};

#define EPOLLONESHOT (1u << 30)
/* EPOLLET (1u << 31) needs nothing: see above */

static uint16_t get16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const unsigned char* p) {
    return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}
static uint64_t get64(const unsigned char* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

/* a timespec {s64 sec, s64 nsec} as milliseconds, rounded up; -1 for none */
static int timespec_ms(GnuMemory* m, uint64_t a, int* ms) {
    *ms = -1;
    if (!a) return 0;
    unsigned char t[16];
    int e = gnu_memory_read(m, a, t, 16);
    if (e) return e;
    int64_t sec = (int64_t)get64(t), nsec = (int64_t)get64(t + 8);
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) return -GNU_EINVAL;
    int64_t total = sec > 0x7FFFFFFF / 1000 ? 0x7FFFFFFF : sec * 1000 + (nsec + 999999) / 1000000;
    *ms = total > 0x7FFFFFFF ? 0x7FFFFFFF : (int)total;
    return 0;
}

/* One wait over descriptors. events and revents are the stack's (SNX_POLL*). A descriptor that isn't open gets
   NVAL, as poll() reports it */
typedef struct {
    int fd;
    uint16_t events, revents;
} Wait;
static int64_t wait_fds(GnuProcess* p, Wait* w, size_t n, int ms) {
    SnxPollEntry* e = calloc(n ? n : 1, sizeof *e);
    uint16_t* fixed = calloc(n ? n : 1, sizeof *fixed);
    if (!e || !fixed) {
        free(e);
        free(fixed);
        return -GNU_ENOMEM;
    }
    int ready = 0;
    for (size_t i = 0; i < n; i++) {
        GnuFdInfo info;
        e[i].host_fd = -1;
        e[i].events = w[i].events;
        if (w[i].fd < 0) continue;
        if (gnu_vfs_fd_info(&p->vfs, w[i].fd, &info))
            fixed[i] = SNX_POLLNVAL;
        else if ((e[i].socket = gnu_net_pollable(&info)))
            continue;
        else if (info.host_fd >= 0) {
#ifdef _WIN32
            fixed[i] = w[i].events & (SNX_POLLIN | SNX_POLLOUT | SNX_POLLRDNORM | SNX_POLLWRNORM);
#else
            e[i].host_fd = info.host_fd;
#endif
        } else if (info.regular)
            fixed[i] = w[i].events & (SNX_POLLIN | SNX_POLLOUT | SNX_POLLRDNORM | SNX_POLLWRNORM);
        /* an epoll descriptor in a poll set never reports */
        ready |= fixed[i] != 0;
    }
    int64_t r = snx_net_poll(e, n, ready ? 0 : ms);
    if (r >= 0) {
        r = 0;
        for (size_t i = 0; i < n; i++) {
            w[i].revents = (uint16_t)(e[i].revents | fixed[i]);
            r += w[i].revents != 0;
        }
    }
    free(e);
    free(fixed);
    return r;
}

int64_t gnu_net_ppoll(GnuProcess* p, const uint64_t a[6]) {
    uint64_t n = a[1];
    int ms, e;
    if (n > POLL_MAX) return -GNU_EINVAL;
    if ((e = timespec_ms(&p->memory, a[2], &ms))) return e;
    unsigned char* fds = n ? gnu_memory_access(&p->memory, a[0], (size_t)n * 8, GNU_PROT_READ | GNU_PROT_WRITE) : NULL;
    if (n && !fds) return -GNU_EFAULT;
    Wait* w = calloc(n ? (size_t)n : 1, sizeof *w);
    if (!w) return -GNU_ENOMEM;
    for (uint64_t i = 0; i < n; i++) { /* pollfd {s32 fd, s16 events, s16 revents} */
        w[i].fd = (int32_t)get32(fds + i * 8);
        w[i].events = gnu_net_poll_in(get16(fds + i * 8 + 4));
    }
    int64_t r = wait_fds(p, w, (size_t)n, ms);
    for (uint64_t i = 0; r >= 0 && i < n; i++) {
        /* ERR, HUP and NVAL come whether asked or not */
        uint16_t rev = w[i].revents & (w[i].events | SNX_POLLERR | SNX_POLLHUP | SNX_POLLNVAL);
        uint32_t g = gnu_net_poll_out(rev);
        fds[i * 8 + 6] = (unsigned char)g;
        fds[i * 8 + 7] = (unsigned char)(g >> 8);
    }
    free(w);
    return r == -SNX_EINTR ? -GNU_EINTR : r;
}

int64_t gnu_net_pselect6(GnuProcess* p, const uint64_t a[6]) {
    int64_t nfds = (int64_t)a[0];
    int ms, e;
    if (nfds < 0 || nfds > SELECT_MAX) return -GNU_EINVAL;
    if ((e = timespec_ms(&p->memory, a[4], &ms))) return e;
    size_t bytes = (size_t)((nfds + 63) / 64 * 8); /* fd_set is an array of unsigned long */
    unsigned char* set[3];
    for (int k = 0; k < 3; k++) {
        set[k] =
            a[1 + k] && bytes ? gnu_memory_access(&p->memory, a[1 + k], bytes, GNU_PROT_READ | GNU_PROT_WRITE) : NULL;
        if (a[1 + k] && bytes && !set[k]) return -GNU_EFAULT;
    }
    Wait w[SELECT_MAX];
    size_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint16_t ev = 0;
        if (set[0] && (set[0][fd / 8] >> (fd % 8) & 1)) ev |= SNX_POLLIN;
        if (set[1] && (set[1][fd / 8] >> (fd % 8) & 1)) ev |= SNX_POLLOUT;
        if (set[2] && (set[2][fd / 8] >> (fd % 8) & 1)) ev |= SNX_POLLPRI;
        if (!ev) continue;
        w[n].fd = fd;
        w[n++].events = ev;
    }
    int64_t r = wait_fds(p, w, n, ms);
    if (r < 0) return r == -SNX_EINTR ? -GNU_EINTR : r;
    for (size_t i = 0; i < n; i++)
        if (w[i].revents & SNX_POLLNVAL) return -GNU_EBADF;
    for (int k = 0; k < 3; k++)
        if (set[k]) memset(set[k], 0, bytes);
    r = 0;
    for (size_t i = 0; i < n; i++) {
        int fd = w[i].fd;
        uint16_t rev = w[i].revents;
        int hit[3] = {(w[i].events & SNX_POLLIN) && (rev & (SNX_POLLIN | SNX_POLLHUP | SNX_POLLERR)),
                      (w[i].events & SNX_POLLOUT) && (rev & (SNX_POLLOUT | SNX_POLLHUP | SNX_POLLERR)),
                      (w[i].events & SNX_POLLPRI) && (rev & SNX_POLLPRI)};
        for (int k = 0; k < 3; k++)
            if (hit[k]) {
                set[k][fd / 8] |= (unsigned char)(1 << (fd % 8));
                r++;
            }
    }
    return r;
}

/* ============================================================ epoll */
typedef struct {
    int fd;
    uint32_t events; /* the guest's, with EPOLLET and EPOLLONESHOT */
    uint64_t data;
    const void* identity; /* the open file registered: closing it ends the registration, as on Linux */
    int disarmed;
} EpollItem;
typedef struct {
    EpollItem* items;
    size_t count, capacity;
    GnuMutex lock;
} Epoll;
static void epoll_release(void* o) {
    Epoll* ep = o;
    gnu_mutex_destroy(&ep->lock);
    free(ep->items);
    free(ep);
}
static const GnuObjectOps g_epoll_ops = {"anon_inode:[eventpoll]", 0, NULL, NULL, NULL, epoll_release};

int64_t gnu_net_epoll_create1(GnuProcess* p, uint64_t flags) {
    if (flags & ~(uint64_t)EPOLL_CLOEXEC) return -GNU_EINVAL;
    Epoll* ep = calloc(1, sizeof *ep);
    if (!ep) return -GNU_ENOMEM;
    gnu_mutex_init(&ep->lock, 0);
    return gnu_vfs_install_object(&p->vfs, &g_epoll_ops, ep, 0, (flags & EPOLL_CLOEXEC) != 0);
}
/* the entry for fd that still names the file registered; entries for closed files go */
static EpollItem* epoll_find(GnuProcess* p, Epoll* ep, int fd) {
    for (size_t i = 0; i < ep->count; i++) {
        GnuFdInfo info;
        if (gnu_vfs_fd_info(&p->vfs, ep->items[i].fd, &info) || info.identity != ep->items[i].identity) {
            ep->items[i--] = ep->items[--ep->count];
            continue;
        }
        if (ep->items[i].fd == fd) return &ep->items[i];
    }
    return NULL;
}
static int64_t epoll_ctl_locked(GnuProcess* p, Epoll* ep, const uint64_t a[6]);
int64_t gnu_net_epoll_ctl(GnuProcess* p, const uint64_t a[6]) {
    Epoll* ep = gnu_vfs_object(&p->vfs, (int)a[0], &g_epoll_ops, NULL);
    GnuFdInfo info;
    if (!ep) return gnu_vfs_fd_info(&p->vfs, (int)a[0], &info) ? -GNU_EBADF : -GNU_EINVAL;
    gnu_mutex_lock(&ep->lock);
    int64_t r = epoll_ctl_locked(p, ep, a);
    gnu_mutex_unlock(&ep->lock);
    return r;
}
static int64_t epoll_ctl_locked(GnuProcess* p, Epoll* ep, const uint64_t a[6]) {
    GnuFdInfo info;
    int fd = (int)a[2];
    if (gnu_vfs_fd_info(&p->vfs, fd, &info)) return -GNU_EBADF;
    if (fd == (int)a[0]) return -GNU_EINVAL;
    if (info.regular) return -GNU_EPERM; /* Linux: files and directories can't be polled for readiness */
    unsigned char ev[EPOLL_EVENT_SIZE] = {0};
    if (a[1] != EPOLL_CTL_DEL) {
        int e = gnu_memory_read(&p->memory, a[3], ev, sizeof ev);
        if (e) return e;
    }
    EpollItem* it = epoll_find(p, ep, fd);
    switch (a[1]) {
        case EPOLL_CTL_ADD:
            if (it) return -GNU_EEXIST;
            if (ep->count == ep->capacity) {
                size_t cap = ep->capacity ? ep->capacity * 2 : 16;
                EpollItem* items = realloc(ep->items, cap * sizeof *items);
                if (!items) return -GNU_ENOMEM;
                ep->items = items;
                ep->capacity = cap;
            }
            it = &ep->items[ep->count++];
            it->fd = fd;
            it->identity = info.identity;
            /* fallthrough */
        case EPOLL_CTL_MOD:
            if (!it) return -GNU_ENOENT;
            it->events = get32(ev);
            it->data = get64(ev + 8);
            it->disarmed = 0;
            return 0;
        case EPOLL_CTL_DEL:
            if (!it) return -GNU_ENOENT;
            *it = ep->items[--ep->count];
            return 0;
    }
    return -GNU_EINVAL;
}
int64_t gnu_net_epoll_pwait(GnuProcess* p, const uint64_t a[6], int timespec_timeout) {
    Epoll* ep = gnu_vfs_object(&p->vfs, (int)a[0], &g_epoll_ops, NULL);
    GnuFdInfo info;
    if (!ep) return gnu_vfs_fd_info(&p->vfs, (int)a[0], &info) ? -GNU_EBADF : -GNU_EINVAL;
    int max = (int)a[2], ms = (int)a[3];
    if (max <= 0 || max > 0x7FFFFFFF / EPOLL_EVENT_SIZE) return -GNU_EINVAL;
    if (timespec_timeout) {
        int e = timespec_ms(&p->memory, a[3], &ms);
        if (e) return e;
    }
    unsigned char* out = gnu_memory_access(&p->memory, a[1], (size_t)max * EPOLL_EVENT_SIZE, GNU_PROT_WRITE);
    if (!out) return -GNU_EFAULT;
    gnu_mutex_lock(&ep->lock);
    epoll_find(p, ep, -1); /* drops the entries of closed files */
    Wait* w = calloc(ep->count ? ep->count : 1, sizeof *w);
    size_t* which = calloc(ep->count ? ep->count : 1, sizeof *which);
    if (!w || !which) {
        gnu_mutex_unlock(&ep->lock);
        free(w);
        free(which);
        return -GNU_ENOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < ep->count; i++) {
        if (ep->items[i].disarmed) continue;
        w[n].fd = ep->items[i].fd;
        w[n].events = gnu_net_poll_in(ep->items[i].events);
        which[n++] = i;
    }
    gnu_mutex_unlock(&ep->lock);
    int64_t r = n ? wait_fds(p, w, n, ms) : 0;
    if (!n && ms) {
        /* nothing to watch: wait out the timeout on no descriptor, which only a cancel ends */
        SnxPollEntry none = {NULL, -1, 0, 0};
        r = snx_net_poll(&none, 1, ms);
    }
    int64_t count = 0;
    gnu_mutex_lock(&ep->lock);
    for (size_t i = 0; r > 0 && i < n && count < max; i++) {
        uint16_t rev = w[i].revents & (w[i].events | SNX_POLLERR | SNX_POLLHUP);
        /* an entry another thread took out during the wait is not reported */
        if (!rev || which[i] >= ep->count || ep->items[which[i]].fd != w[i].fd) continue;
        EpollItem* it = &ep->items[which[i]];
        unsigned char* e = out + count++ * EPOLL_EVENT_SIZE;
        memset(e, 0, EPOLL_EVENT_SIZE);
        gnu_vfs_put32(e, gnu_net_poll_out(rev));
        gnu_vfs_put64(e + 8, it->data);
        if (it->events & EPOLLONESHOT) it->disarmed = 1;
    }
    gnu_mutex_unlock(&ep->lock);
    free(w);
    free(which);
    if (r < 0) return r == -SNX_EINTR ? -GNU_EINTR : r;
    return count;
}
