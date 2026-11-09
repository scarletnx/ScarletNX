/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/internal_network/network.cpp and sockets.h (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project, GPL-2.0-or-later
   Eden's host sockets block in the host's calls, and only accept() polls its interrupt pipe. Here host sockets
   never block and every wait polls the cancel descriptor, so snx_net_cancel() wakes any call, and SO_RCVTIMEO and
   SO_SNDTIMEO are the stack's own. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* POLLRDHUP, accept4 and SOCK_CLOEXEC on glibc */
#endif
#include "common/network/net_internal.h"
#include "common/platform/thread.h"
#include "common/utilities/endian.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <mstcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ============================================================ host constants a host lacks (the tables in abi/) */
#define SNX_NET_NOOPT (-1)
#ifndef MSG_EOR
#define MSG_EOR 0
#endif
#ifndef MSG_TRUNC
#define MSG_TRUNC 0
#endif
#ifndef MSG_NOSIGNAL
#define SNX_SEND_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on every socket instead. Windows has no SIGPIPE */
#else
#define SNX_SEND_NOSIGNAL MSG_NOSIGNAL
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif
#ifdef SO_NOSIGPIPE
#define SNX_HOST_NOSIGPIPE 1
#else
#define SO_NOSIGPIPE SNX_NET_NOOPT
#endif
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SNX_NET_NOOPT
#endif
#ifndef SO_PROTOCOL
#define SO_PROTOCOL SNX_NET_NOOPT
#endif
#ifndef SO_DOMAIN
#define SO_DOMAIN SNX_NET_NOOPT
#endif
#if defined(__APPLE__) && !defined(TCP_KEEPIDLE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE SNX_NET_NOOPT
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL SNX_NET_NOOPT
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT SNX_NET_NOOPT
#endif
#ifndef TCP_MAXSEG
#define TCP_MAXSEG SNX_NET_NOOPT
#endif
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

/* ============================================================ messages */
static void log_stderr(const char* m) { fputs(m, stderr); }
static void (*g_log)(const char*) = log_stderr;
static void (*g_warn)(const char*) = log_stderr;
void snx_net_set_log(void (*log)(const char* message)) { g_log = log ? log : log_stderr; }
void snx_net_set_warn(void (*warn)(const char* message)) { g_warn = warn ? warn : log_stderr; }
static void say(void (*to)(const char*), const char* fmt, va_list ap) {
    char m[512];
    int n = vsnprintf(m, sizeof m - 1, fmt, ap);
    if (n < 0) return;
    size_t len = strlen(m);
    if (!len || m[len - 1] != '\n') strcat(m, "\n");
    to(m);
}
void snx_net_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(g_log, fmt, ap);
    va_end(ap);
}
void snx_net_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(g_warn, fmt, ap);
    va_end(ap);
}

/* ============================================================ locks, threads, time, notifiers */
/* the system's default stack: libc TLS and sanitizer runtimes may need more than a small fixed one */
int snx_net_thread(void (*fn)(void*), void* arg) { return plat_thread(fn, arg, 0) ? -SNX_EAGAIN : 0; }
#ifdef _WIN32
void snx_lock_init(SnxLock* l) { InitializeSRWLock(l); }
void snx_lock_destroy(SnxLock* l) { (void)l; }
void snx_lock(SnxLock* l) { AcquireSRWLockExclusive(l); }
void snx_unlock(SnxLock* l) { ReleaseSRWLockExclusive(l); }
int snx_net_close_fd(SnxHostFd fd) { return closesocket(fd) ? snx_net_host_error() : 0; }
int snx_net_set_host_nonblocking(SnxHostFd fd) {
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) ? snx_net_host_error() : 0;
}
/* a UDP pair on loopback: WSAPoll only polls sockets */
int snx_notifier_open(SnxHostFd out[2]) {
    SnxHostFd a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa, sb;
    int la = sizeof sa, lb = sizeof sb;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sb = sa;
    if (a == INVALID_SOCKET || b == INVALID_SOCKET || bind(a, (struct sockaddr*)&sa, sizeof sa) ||
        bind(b, (struct sockaddr*)&sb, sizeof sb) || getsockname(a, (struct sockaddr*)&sa, &la) ||
        getsockname(b, (struct sockaddr*)&sb, &lb) || connect(a, (struct sockaddr*)&sb, sizeof sb) ||
        connect(b, (struct sockaddr*)&sa, sizeof sa) || snx_net_set_host_nonblocking(a) ||
        snx_net_set_host_nonblocking(b)) {
        int e = snx_net_host_error();
        if (a != INVALID_SOCKET) closesocket(a);
        if (b != INVALID_SOCKET) closesocket(b);
        return e;
    }
    out[0] = a;
    out[1] = b;
    return 0;
}
void snx_notifier_signal(SnxHostFd w) {
    char one = 1;
    send(w, &one, 1, 0);
}
void snx_notifier_drain(SnxHostFd r) {
    char buf[64];
    while (recv(r, buf, sizeof buf, 0) > 0) {}
}
#else
void snx_lock_init(SnxLock* l) { pthread_mutex_init(l, NULL); }
void snx_lock_destroy(SnxLock* l) { pthread_mutex_destroy(l); }
void snx_lock(SnxLock* l) { pthread_mutex_lock(l); }
void snx_unlock(SnxLock* l) { pthread_mutex_unlock(l); }
int snx_net_close_fd(SnxHostFd fd) { return close(fd) ? snx_net_host_error() : 0; }
static void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }
int snx_net_set_host_nonblocking(SnxHostFd fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return snx_net_host_error();
    set_cloexec(fd);
    return 0;
}
int snx_notifier_open(SnxHostFd out[2]) {
    int p[2];
    if (pipe(p)) return snx_net_host_error();
    snx_net_set_host_nonblocking(p[0]);
    snx_net_set_host_nonblocking(p[1]);
    out[0] = p[0];
    out[1] = p[1];
    return 0;
}
void snx_notifier_signal(SnxHostFd w) {
    char one = 1;
    while (write(w, &one, 1) < 0 && errno == EINTR) {}
}
void snx_notifier_drain(SnxHostFd r) {
    char buf[64];
    while (read(r, buf, sizeof buf) > 0) {}
}
#endif

/* ============================================================ start, cancel */
static SnxHostFd g_cancel[2] = {SNX_BADFD, SNX_BADFD};
static int g_init_result = 1; /* 1: not started */
static void init_once(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        g_init_result = -SNX_ENETDOWN;
        return;
    }
#endif
    g_init_result = snx_notifier_open(g_cancel);
    if (g_init_result) snx_net_log("net: no cancel pipe (%d), blocking calls can't be interrupted", g_init_result);
    g_init_result = 0;
}
#ifdef _WIN32
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_once_win(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o, (void)p, (void)c;
    init_once();
    return TRUE;
}
int snx_net_init(void) {
    InitOnceExecuteOnce(&g_once, init_once_win, NULL, NULL);
    return g_init_result;
}
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
int snx_net_init(void) {
    pthread_once(&g_once, init_once);
    return g_init_result;
}
#endif
struct SnxNetWake {
    SnxHostFd fd[2];
};
static _Thread_local struct {
    SnxNetWake* wake; /* snx_net_wake_bind's */
} t_net;
SnxNetWake* snx_net_wake_new(void) {
    SnxNetWake* w = calloc(1, sizeof *w);
    if (w && (snx_net_init() || snx_notifier_open(w->fd))) {
        free(w);
        return NULL;
    }
    return w;
}
void snx_net_wake_free(SnxNetWake* w) {
    if (!w) return;
    if (t_net.wake == w) t_net.wake = NULL;
    snx_net_close_fd(w->fd[0]);
    snx_net_close_fd(w->fd[1]);
    free(w);
}
void snx_net_wake_signal(SnxNetWake* w) {
    if (w) snx_notifier_signal(w->fd[1]);
}
void snx_net_wake_drain(SnxNetWake* w) {
    if (w) snx_notifier_drain(w->fd[0]);
}
void snx_net_wake_bind(SnxNetWake* w) { t_net.wake = w; }

void snx_net_cancel(void) {
    if (!snx_net_init() && g_cancel[1] != SNX_BADFD) snx_notifier_signal(g_cancel[1]);
}
void snx_net_restart(void) {
    if (!snx_net_init() && g_cancel[0] != SNX_BADFD) snx_notifier_drain(g_cancel[0]);
}

/* ============================================================ errors */
int snx_net_errno_from_host(int e) {
    if (!e) return 0;
#ifdef _WIN32
#define SNX_NET_WSAERR(host, name)                                                                                     \
    if (e == host) return -SNX_E##name;
#include "common/network/abi/errno_win32.inc"
#else
#define SNX_NET_ERRNO(name, value)                                                                                     \
    if (e == E##name) return -SNX_E##name;
#include "common/network/abi/errno.inc"
    if (e == EWOULDBLOCK) return -SNX_EAGAIN;
#ifdef ENOTSUP
    if (e == ENOTSUP) return -SNX_EOPNOTSUPP;
#endif
#endif
    snx_net_log("net: host error %d has no translation, reporting EIO", e);
    return -SNX_EIO;
}
int snx_net_host_error(void) {
#ifdef _WIN32
    return snx_net_errno_from_host(WSAGetLastError());
#else
    return snx_net_errno_from_host(errno);
#endif
}

/* ============================================================ constants */
int snx_net_domain_to_host(int d) {
#define SNX_NET_DOMAIN(name, value, host)                                                                              \
    if (d == value) return host;
#include "common/network/abi/domain.inc"
    return -1;
}
int snx_net_domain_from_host(int d) {
#define SNX_NET_DOMAIN(name, value, host)                                                                              \
    if (d == host) return value;
#include "common/network/abi/domain.inc"
    return -1;
}
int snx_net_socktype_to_host(int t) {
#define SNX_NET_SOCKTYPE(name, value, host)                                                                            \
    if (t == value) return host;
#include "common/network/abi/socktype.inc"
    return -1;
}
int snx_net_socktype_from_host(int t) {
#define SNX_NET_SOCKTYPE(name, value, host)                                                                            \
    if (t == host) return value;
#include "common/network/abi/socktype.inc"
    return 0;
}
int snx_net_level_to_host(int l) {
#define SNX_NET_LEVEL(name, value, host)                                                                               \
    if (l == value) return host;
#include "common/network/abi/level.inc"
    return -1;
}
int snx_net_msg_to_host(int f) {
    int h = 0;
#define SNX_NET_MSG(name, value, host)                                                                                 \
    if (f & value) h |= host;
#include "common/network/abi/msgflag.inc"
    return h;
}
/* Winsock's recvfrom reports no flags: unused there */
int snx_net_msg_from_host(int h) {
    int f = 0;
#define SNX_NET_MSG(name, value, host)                                                                                 \
    if ((host) && (h & (host))) f |= value;
#include "common/network/abi/msgflag.inc"
    return f;
}
short snx_net_poll_to_host(unsigned e) {
    short h = 0;
#ifdef _WIN32
#define SNX_NET_POLL(name, value, host, wsapoll)                                                                       \
    if ((e & value) && wsapoll) h |= host;
#else
#define SNX_NET_POLL(name, value, host, wsapoll)                                                                       \
    if (e & value) h |= host;
#endif
#include "common/network/abi/poll.inc"
    return h;
}
uint16_t snx_net_poll_from_host(short h) {
    uint16_t e = 0;
#define SNX_NET_POLL(name, value, host, wsapoll)                                                                       \
    if ((host) && (h & (host))) e |= value;
#include "common/network/abi/poll.inc"
    return e;
}

uint16_t snx_net_checksum(const void* data, size_t size, uint32_t sum) {
    const uint8_t* p = data;
    for (size_t i = 0; i + 1 < size; i += 2) sum += snx_ld_be16(p + i);
    if (size & 1) sum += (uint32_t)p[size - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ============================================================ addresses */
int snx_net_addr_to_host(const SnxSockAddr* in, struct sockaddr_storage* out, snx_socklen* size) {
    /* each address is made as its own type and copied in: stores through a sockaddr_in into a sockaddr_storage would
       be another type's object, which the optimizer may assume they don't touch */
    memset(out, 0, sizeof *out);
    switch (in->family) {
        case SNX_AF_INET: {
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = in->port;
            memcpy(&a.sin_addr, in->addr, 4);
#if defined(__APPLE__) || defined(__FreeBSD__)
            a.sin_len = sizeof a;
#endif
            memcpy(out, &a, sizeof a);
            *size = sizeof a;
            return 0;
        }
        case SNX_AF_INET6: {
            struct sockaddr_in6 a;
            memset(&a, 0, sizeof a);
            a.sin6_family = AF_INET6;
            a.sin6_port = in->port;
            a.sin6_flowinfo = in->flowinfo;
            memcpy(&a.sin6_addr, in->addr, 16);
            a.sin6_scope_id = in->scope_id;
#if defined(__APPLE__) || defined(__FreeBSD__)
            a.sin6_len = sizeof a;
#endif
            memcpy(out, &a, sizeof a);
            *size = sizeof a;
            return 0;
        }
        case SNX_AF_UNSPEC: /* connect() with it dissolves a datagram socket's association */
            out->ss_family = AF_UNSPEC;
            *size = sizeof(struct sockaddr);
            return 0;
    }
    return -SNX_EAFNOSUPPORT;
}
int snx_net_addr_from_host(const struct sockaddr* in, snx_socklen size, SnxSockAddr* out) {
    /* read as bytes: in is whichever type the system wrote, a sockaddr_storage or the address's own */
    struct sockaddr head;
    memset(out, 0, sizeof *out);
    memset(&head, 0, sizeof head);
    if (size > 0) memcpy(&head, in, (size_t)size < sizeof head ? (size_t)size : sizeof head);
    int family = head.sa_family;
    if (size >= (snx_socklen)sizeof(struct sockaddr_in) && family == AF_INET) {
        struct sockaddr_in a;
        memcpy(&a, in, sizeof a);
        out->family = SNX_AF_INET;
        out->port = a.sin_port;
        memcpy(out->addr, &a.sin_addr, 4);
        return 0;
    }
    if (size >= (snx_socklen)sizeof(struct sockaddr_in6) && family == AF_INET6) {
        struct sockaddr_in6 a;
        memcpy(&a, in, sizeof a);
        out->family = SNX_AF_INET6;
        out->port = a.sin6_port;
        out->flowinfo = a.sin6_flowinfo;
        memcpy(out->addr, &a.sin6_addr, 16);
        out->scope_id = a.sin6_scope_id;
        return 0;
    }
    if (size > 0 && family == AF_UNIX) {
        out->family = SNX_AF_UNIX; /* unnamed: socketpair()'s ends */
        return 0;
    }
    return -SNX_EAFNOSUPPORT;
}

/* ============================================================ waiting */
#ifdef _WIN32
typedef WSAPOLLFD HostPollFd;
static int host_poll(HostPollFd* f, size_t n, int ms) { return WSAPoll(f, (ULONG)n, ms); }
#else
typedef struct pollfd HostPollFd;
static int host_poll(HostPollFd* f, size_t n, int ms) { return poll(f, (nfds_t)n, ms); }
#endif
static int interrupted(int r) {
#ifdef _WIN32
    return r < 0 && WSAGetLastError() == WSAEINTR;
#else
    return r < 0 && errno == EINTR;
#endif
}
/* milliseconds left until deadline (0: none), rounded up so a wait never ends early; -1 forever */
static int ms_left(int64_t deadline) {
    if (!deadline) return -1;
    int64_t left = deadline - snx_net_now_us();
    if (left <= 0) return 0;
    left = (left + 999) / 1000;
    return left > INT_MAX ? INT_MAX : (int)left;
}
int snx_net_wait_until(SnxHostFd fd, short events, int64_t deadline) {
    for (;;) {
        HostPollFd f[3] = {0};
        f[0].fd = fd;
        f[0].events = events;
        size_t n = 1;
        if (g_cancel[0] != SNX_BADFD) {
            f[n].fd = g_cancel[0];
            f[n++].events = POLLIN;
        }
        if (t_net.wake) {
            f[n].fd = t_net.wake->fd[0];
            f[n++].events = POLLIN;
        }
        int ms = ms_left(deadline);
        int r = host_poll(f, n, ms);
        if (interrupted(r)) continue;
        if (r < 0) return snx_net_host_error();
        for (size_t i = 1; i < n; i++)
            if (f[i].revents) return -SNX_EINTR;
        if (f[0].revents) return 1;
        if (!ms) return -SNX_EAGAIN;
    }
}
int snx_net_wait(SnxHostFd fd, short events, int64_t timeout_us) {
    return snx_net_wait_until(fd, events, timeout_us > 0 ? snx_net_now_us() + timeout_us : 0);
}
int64_t snx_net_deadline(int64_t timeout_us) { return timeout_us > 0 ? snx_net_now_us() + timeout_us : 0; }

/* ============================================================ host sockets */
static const SnxSocketOps g_host_ops;
SnxSocket* snx_net_socket_new(const SnxSocketOps* ops, SnxHostFd fd, int domain, int type, int protocol) {
    SnxSocket* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ops = ops;
    s->fd = fd;
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->refs = 1;
    return s;
}
static int prepare_host_fd(SnxHostFd fd) {
    int r = snx_net_set_host_nonblocking(fd);
#ifdef SNX_HOST_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return r;
}
static void host_destroy(SnxSocket* s) {
    if (s->fd != SNX_BADFD) snx_net_close_fd(s->fd);
}
static int host_bind(SnxSocket* s, const SnxSockAddr* a) {
    struct sockaddr_storage ss;
    snx_socklen n;
    int r = snx_net_addr_to_host(a, &ss, &n);
    if (r) return r;
    return bind(s->fd, (struct sockaddr*)&ss, n) ? snx_net_host_error() : 0;
}
static int pending_error(SnxSocket* s) {
    int e = 0;
    snx_socklen n = sizeof e;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char*)&e, &n)) return snx_net_host_error();
    return snx_net_errno_from_host(e);
}
static int host_connect(SnxSocket* s, const SnxSockAddr* a) {
    struct sockaddr_storage ss;
    snx_socklen n;
    int r = snx_net_addr_to_host(a, &ss, &n);
    if (r) return r;
    if (!connect(s->fd, (struct sockaddr*)&ss, n)) return 0;
#ifdef _WIN32
    int e = WSAGetLastError();
    r = e == WSAEWOULDBLOCK ? -SNX_EINPROGRESS : snx_net_errno_from_host(e);
#else
    r = snx_net_host_error();
#endif
    if (r != -SNX_EINPROGRESS || s->nonblocking) return r;
    r = snx_net_wait(s->fd, POLLOUT, s->sndtimeo_us);
    if (r < 0) return r == -SNX_EAGAIN ? -SNX_EINPROGRESS : r;
    return pending_error(s);
}
static int host_listen(SnxSocket* s, int backlog) { return listen(s->fd, backlog) ? snx_net_host_error() : 0; }
static int host_accept(SnxSocket* s, SnxSocket** out, SnxSockAddr* peer, int nonblocking) {
    struct sockaddr_storage ss;
    int64_t deadline = snx_net_deadline(s->rcvtimeo_us);
    for (;;) {
        snx_socklen n = sizeof ss;
        SnxHostFd fd = accept(s->fd, (struct sockaddr*)&ss, &n);
        if (fd != SNX_BADFD) {
            prepare_host_fd(fd);
            SnxSocket* c = snx_net_socket_new(&g_host_ops, fd, s->domain, s->type, s->protocol);
            if (!c) {
                snx_net_close_fd(fd);
                return -SNX_ENOMEM;
            }
            c->nonblocking = nonblocking;
            c->rcvtimeo_us = s->rcvtimeo_us;
            c->sndtimeo_us = s->sndtimeo_us;
            if (peer && snx_net_addr_from_host((struct sockaddr*)&ss, n, peer)) memset(peer, 0, sizeof *peer);
            *out = c;
            return 0;
        }
        int r = snx_net_host_error();
        if (r != -SNX_EAGAIN || s->nonblocking) return r;
        r = snx_net_wait_until(s->fd, POLLIN, deadline);
        if (r < 0) return r;
    }
}
static int host_shutdown(SnxSocket* s, int how) {
#ifdef _WIN32
    int h = how == SNX_SHUT_RD ? SD_RECEIVE : how == SNX_SHUT_WR ? SD_SEND : SD_BOTH;
#else
    int h = how == SNX_SHUT_RD ? SHUT_RD : how == SNX_SHUT_WR ? SHUT_WR : SHUT_RDWR;
#endif
    if (how < SNX_SHUT_RD || how > SNX_SHUT_RDWR) return -SNX_EINVAL;
    return shutdown(s->fd, h) ? snx_net_host_error() : 0;
}
static int host_send_error(void) {
#ifdef _WIN32
    /* Winsock reports WSAECONNABORTED where POSIX hosts and the console report EPIPE (Eden) */
    if (WSAGetLastError() == WSAECONNABORTED) return -SNX_EPIPE;
#endif
    return snx_net_host_error();
}
static int64_t host_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to) {
    struct sockaddr_storage ss;
    snx_socklen n = 0;
    if (to) {
        int r = snx_net_addr_to_host(to, &ss, &n);
        if (r) return r;
    }
    int hflags = snx_net_msg_to_host(flags) | SNX_SEND_NOSIGNAL;
    int dontwait = s->nonblocking || (flags & SNX_MSG_DONTWAIT);
    int64_t deadline = snx_net_deadline(s->sndtimeo_us), total = 0;
    const char* p = data;
    for (;;) {
        size_t chunk = size - (size_t)total;
        if (chunk > INT_MAX) chunk = INT_MAX;
#ifdef _WIN32
        int r = sendto(s->fd, p + total, (int)chunk, hflags, to ? (struct sockaddr*)&ss : NULL, n);
#else
        ssize_t r = sendto(s->fd, p + total, chunk, hflags, to ? (struct sockaddr*)&ss : NULL, n);
#endif
        if (r >= 0) {
            total += r;
            /* a blocking stream socket sends everything, a datagram goes out whole or not at all */
            if ((size_t)total >= size || s->type != SNX_SOCK_STREAM || dontwait) return total;
            continue;
        }
        int e = host_send_error();
        if (e != -SNX_EAGAIN || dontwait) return total ? total : e;
        e = snx_net_wait_until(s->fd, POLLOUT, deadline);
        if (e < 0) return total ? total : e;
    }
}
static int64_t host_recv_once(SnxSocket* s, char* p, size_t size, int hflags, SnxSockAddr* from, int* out_flags) {
    struct sockaddr_storage ss;
    if (size > INT_MAX) size = INT_MAX;
#ifdef _WIN32
    int n = sizeof ss;
    int r = recvfrom(s->fd, p, (int)size, hflags, from ? (struct sockaddr*)&ss : NULL, from ? &n : NULL);
    if (r < 0 && WSAGetLastError() == WSAEMSGSIZE && s->type != SNX_SOCK_STREAM) {
        if (out_flags) *out_flags |= SNX_MSG_TRUNC; /* Winsock fails a datagram that doesn't fit, POSIX cuts it */
        r = (int)size;
    }
#else
    struct iovec iov = {p, size};
    struct msghdr m = {0};
    m.msg_name = from ? &ss : NULL;
    m.msg_namelen = from ? sizeof ss : 0;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    ssize_t r = recvmsg(s->fd, &m, hflags);
    snx_socklen n = m.msg_namelen;
    if (r >= 0 && out_flags) *out_flags |= snx_net_msg_from_host(m.msg_flags);
#endif
    if (r >= 0 && from && (n <= 0 || snx_net_addr_from_host((struct sockaddr*)&ss, n, from)))
        memset(from, 0, sizeof *from); /* a connected stream socket names no sender */
    return r < 0 ? snx_net_host_error() : r;
}
static int64_t host_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags) {
    int hflags = snx_net_msg_to_host(flags);
    int dontwait = s->nonblocking || (flags & SNX_MSG_DONTWAIT);
    int waitall = (flags & SNX_MSG_WAITALL) && !(flags & SNX_MSG_PEEK) && s->type == SNX_SOCK_STREAM && !dontwait;
    int64_t deadline = snx_net_deadline(s->rcvtimeo_us), total = 0;
    char* p = data;
    if (out_flags) *out_flags = 0;
    for (;;) {
        int64_t r = host_recv_once(s, p + total, size - (size_t)total, hflags, total ? NULL : from, out_flags);
        if (r >= 0) {
            total += r;
            if (!waitall || !r || (size_t)total >= size) return total;
            continue;
        }
        if (r != -SNX_EAGAIN || dontwait) return total ? total : r;
        r = snx_net_wait_until(s->fd, POLLIN, deadline);
        if (r < 0) return total ? total : r;
    }
}
static int host_name(SnxSocket* s, SnxSockAddr* out, int peer) {
    struct sockaddr_storage ss;
    snx_socklen n = sizeof ss;
    int r = peer ? getpeername(s->fd, (struct sockaddr*)&ss, &n) : getsockname(s->fd, (struct sockaddr*)&ss, &n);
    if (r) return snx_net_host_error();
    if (snx_net_addr_from_host((struct sockaddr*)&ss, n, out)) memset(out, 0, sizeof *out);
    return 0;
}

typedef enum {
    K_INT,
    K_LINGER,
    K_ADDR4,
    K_MREQ,
    K_MREQ6,
    K_RCVTIMEO,
    K_SNDTIMEO,
    K_TYPE,
    K_PROTOCOL,
    K_DOMAIN,
    K_ERROR,
    K_NOOP
} OptKind;
static int opt_lookup(int level, int name, int* hlevel, int* hname, OptKind* kind) {
#define SNX_NET_SOCKOPT(lv_, opt_, value_, kind_)                                                                      \
    if (level == SNX_SOL_##lv_ && name == (value_)) {                                                                  \
        *hlevel = snx_net_level_to_host(SNX_SOL_##lv_);                                                                        \
        *hname = opt_;                                                                                                 \
        *kind = K_##kind_;                                                                                             \
        return 0;                                                                                                      \
    }
#include "common/network/abi/sockopt.inc"
    return -SNX_ENOPROTOOPT;
}
int snx_net_sockopt_to_host(int level, int name, int* host_level, int* host_name) {
    OptKind k;
    int r = opt_lookup(level, name, host_level, host_name, &k);
    if (r) return r;
    return *host_name == SNX_NET_NOOPT || *host_level < 0 ? -SNX_ENOPROTOOPT : 0;
}
int snx_net_sockopt_kept(int level, int name) {
    int hl, hn;
    OptKind k;
    if (opt_lookup(level, name, &hl, &hn, &k)) return 0;
    return k == K_RCVTIMEO || k == K_SNDTIMEO || k == K_TYPE || k == K_PROTOCOL || k == K_DOMAIN || k == K_NOOP;
}
/* An option's value between the guest's byte order (little-endian: both runtimes' guests) and a big-endian host's,
   its numbers swapped in place by its kind: the same both ways. Bytes (addresses, groups) stay as they are */
static void swap_bytes(unsigned char* v, size_t n) {
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned char t = v[i];
        v[i] = v[n - 1 - i];
        v[n - 1 - i] = t;
    }
}
static void guest_order(OptKind k, unsigned char* v, size_t size) {
    switch (k) {
        case K_ADDR4:
        case K_MREQ: return;
        case K_MREQ6:
            if (size >= 20) swap_bytes(v + 16, 4); /* ipv6mr_interface */
            return;
        case K_RCVTIMEO:
        case K_SNDTIMEO:
            if (size >= 8) swap_bytes(v, 8);
            if (size >= 16) swap_bytes(v + 8, 8);
            return;
        case K_LINGER:
            if (size >= 8) swap_bytes(v + 4, 4);
            /* fall through: and the first int */
        default:
            if (size >= 4) swap_bytes(v, 4);
            return;
    }
}
/* getsockopt's result r, its value in the guest's byte order (nothing on a little-endian host) */
static int guest_value(OptKind k, int r, void* v, const size_t* size) {
    if (SNX_HOST_BE && !r) guest_order(k, (unsigned char*)v, *size);
    return r;
}
static int64_t timeval_us(const SnxTimeval* t) {
    if (t->sec < 0) return 1; /* already expired: give up at the first wait */
    if (t->sec > INT64_MAX / 2000000) return INT64_MAX / 2;
    return t->sec * 1000000 + t->usec; /* 0: forever */
}
int snx_net_host_setsockopt(SnxSocket* s, int level, int name, const void* v, size_t size) {
    int hl, hn;
    OptKind k;
    int r = opt_lookup(level, name, &hl, &hn, &k);
    unsigned char host[64];
    if (r) return r;
    if (SNX_HOST_BE) { /* the guest's value as the host's numbers */
        if (size > sizeof host) size = sizeof host;
        memcpy(host, v, size);
        guest_order(k, host, size);
        v = host;
    }
    switch (k) {
        case K_NOOP: return 0;
        case K_TYPE:
        case K_PROTOCOL:
        case K_DOMAIN:
        case K_ERROR: return -SNX_ENOPROTOOPT;
        case K_RCVTIMEO:
        case K_SNDTIMEO: {
            SnxTimeval t;
            if (size < sizeof t) return -SNX_EINVAL;
            memcpy(&t, v, sizeof t);
            if (t.usec < 0 || t.usec >= 1000000) return -SNX_EDOM;
            *(k == K_RCVTIMEO ? &s->rcvtimeo_us : &s->sndtimeo_us) = timeval_us(&t);
            return 0;
        }
        default: break;
    }
    if (hn == SNX_NET_NOOPT || hl < 0) return -SNX_ENOPROTOOPT;
    union {
        int i;
        struct linger l;
        struct in_addr a;
        struct ip_mreq m;
        struct ipv6_mreq m6;
    } h;
    snx_socklen hs;
    memset(&h, 0, sizeof h);
    switch (k) {
        case K_INT:
            if (size < 4) return -SNX_EINVAL;
            memcpy(&h.i, v, 4);
            hs = sizeof h.i;
            break;
        case K_LINGER: {
            SnxLinger g;
            if (size < sizeof g) return -SNX_EINVAL;
            memcpy(&g, v, sizeof g);
            h.l.l_onoff = g.onoff ? 1 : 0;
            h.l.l_linger = g.linger < 0 ? 0 : g.linger > 0xFFFF ? 0xFFFF : g.linger;
            hs = sizeof h.l;
            break;
        }
        case K_ADDR4:
            if (size < 4) return -SNX_EINVAL;
            memcpy(&h.a, v, 4);
            hs = sizeof h.a;
            break;
        case K_MREQ: {
            SnxIpMreq g;
            if (size < sizeof g) return -SNX_EINVAL;
            memcpy(&g, v, sizeof g);
            memcpy(&h.m.imr_multiaddr, g.multiaddr, 4);
            memcpy(&h.m.imr_interface, g.interface, 4);
            hs = sizeof h.m;
            break;
        }
        case K_MREQ6: {
            SnxIpv6Mreq g;
            if (size < sizeof g) return -SNX_EINVAL;
            memcpy(&g, v, sizeof g);
            memcpy(&h.m6.ipv6mr_multiaddr, g.multiaddr, 16);
            h.m6.ipv6mr_interface = g.ifindex;
            hs = sizeof h.m6;
            break;
        }
        default: return -SNX_ENOPROTOOPT;
    }
    return setsockopt(s->fd, hl, hn, (const char*)&h, hs) ? snx_net_host_error() : 0;
}
static int put_opt(void* v, size_t* size, const void* src, size_t n) {
    if (*size < n) n = *size;
    memcpy(v, src, n);
    *size = n;
    return 0;
}
int snx_net_host_getsockopt(SnxSocket* s, int level, int name, void* v, size_t* size) {
    int hl, hn;
    OptKind k;
    int r = opt_lookup(level, name, &hl, &hn, &k);
    if (r) return r;
    int32_t i = 0;
    switch (k) {
        case K_NOOP: i = 1; return guest_value(k, put_opt(v, size, &i, 4), v, size);
        case K_TYPE: i = s->type; return guest_value(k, put_opt(v, size, &i, 4), v, size);
        case K_PROTOCOL: i = s->protocol; return guest_value(k, put_opt(v, size, &i, 4), v, size);
        case K_DOMAIN: i = s->domain; return guest_value(k, put_opt(v, size, &i, 4), v, size);
        case K_ERROR:
            i = pending_error(s);
            i = -i;
            return guest_value(k, put_opt(v, size, &i, 4), v, size);
        case K_RCVTIMEO:
        case K_SNDTIMEO: {
            int64_t us = k == K_RCVTIMEO ? s->rcvtimeo_us : s->sndtimeo_us;
            SnxTimeval t = {us / 1000000, us % 1000000};
            return guest_value(k, put_opt(v, size, &t, sizeof t), v, size);
        }
        case K_MREQ:
        case K_MREQ6: return -SNX_ENOPROTOOPT;
        default: break;
    }
    if (hn == SNX_NET_NOOPT || hl < 0) return -SNX_ENOPROTOOPT;
    union {
        int i;
        struct linger l;
        struct in_addr a;
    } h;
    memset(&h, 0, sizeof h);
    snx_socklen hs = k == K_LINGER ? sizeof h.l : k == K_ADDR4 ? sizeof h.a : sizeof h.i;
    if (getsockopt(s->fd, hl, hn, (char*)&h, &hs)) return snx_net_host_error();
    if (k == K_LINGER) {
        SnxLinger g = {h.l.l_onoff ? 1 : 0, h.l.l_linger};
        return guest_value(k, put_opt(v, size, &g, sizeof g), v, size);
    }
    if (k == K_ADDR4) return guest_value(k, put_opt(v, size, &h.a, 4), v, size);
    i = h.i;
    return guest_value(k, put_opt(v, size, &i, 4), v, size);
}
static int host_readable_bytes(SnxSocket* s, int* bytes) {
#ifdef _WIN32
    u_long n = 0;
    if (ioctlsocket(s->fd, FIONREAD, &n)) return snx_net_host_error();
#else
    int n = 0;
    if (ioctl(s->fd, FIONREAD, &n)) return snx_net_host_error();
#endif
    *bytes = (int)n;
    return 0;
}
static const SnxSocketOps g_host_ops = {.destroy = host_destroy,
                                        .bind = host_bind,
                                        .connect = host_connect,
                                        .listen = host_listen,
                                        .accept = host_accept,
                                        .shutdown = host_shutdown,
                                        .send = host_send,
                                        .recv = host_recv,
                                        .name = host_name,
                                        .setsockopt = snx_net_host_setsockopt,
                                        .getsockopt = snx_net_host_getsockopt,
                                        .readable_bytes = host_readable_bytes};

static int open_host(SnxSocket** out, int domain, int type, int protocol) {
    int hd = snx_net_domain_to_host(domain), ht = snx_net_socktype_to_host(type);
    if (hd < 0) return -SNX_EAFNOSUPPORT;
    if (ht < 0) return -SNX_ESOCKTNOSUPPORT;
#ifdef SOCK_CLOEXEC
    ht |= SOCK_CLOEXEC;
#endif
    SnxHostFd fd = socket(hd, ht, protocol);
    if (fd == SNX_BADFD) return snx_net_host_error();
    prepare_host_fd(fd);
    *out = snx_net_socket_new(&g_host_ops, fd, domain, type, protocol);
    if (!*out) {
        snx_net_close_fd(fd);
        return -SNX_ENOMEM;
    }
    return 0;
}

/* ============================================================ the interface */
int snx_socket_open(SnxSocket** out, int domain, int type, int protocol) {
    int r = snx_net_init();
    if (r) return r;
    *out = NULL;
    r = open_host(out, domain, type, protocol);
    /* the host wants privileges for raw sockets, and a ping_group_range for ICMP datagram sockets. Eden falls back
       to its emulation the same way, for ICMP only */
    int icmp = (domain == SNX_AF_INET && protocol == SNX_IPPROTO_ICMP) ||
               (domain == SNX_AF_INET6 && protocol == SNX_IPPROTO_ICMPV6);
    if (r && icmp && (type == SNX_SOCK_RAW || type == SNX_SOCK_DGRAM) &&
        (r == -SNX_EPERM || r == -SNX_EACCES || r == -SNX_EPROTONOSUPPORT || r == -SNX_ESOCKTNOSUPPORT))
        r = snx_net_icmp_open(out, domain, type, protocol);
    return r;
}
int snx_socket_pair(SnxSocket* out[2], int domain, int type, int protocol) {
    int r = snx_net_init();
    if (r) return r;
#ifdef _WIN32
    /* no socketpair(): two loopback sockets connected to each other */
    if (domain != SNX_AF_UNIX && domain != SNX_AF_INET) return -SNX_EOPNOTSUPP;
    if (type != SNX_SOCK_STREAM && type != SNX_SOCK_DGRAM) return -SNX_ESOCKTNOSUPPORT;
    (void)protocol;
    SnxSocket *a = NULL, *b = NULL, *l = NULL;
    SnxSockAddr lo = {SNX_AF_INET, 0, 0, {127, 0, 0, 1}, 0}, la, lb;
    if (type == SNX_SOCK_DGRAM) {
        if ((r = open_host(&a, SNX_AF_INET, type, 0)) || (r = open_host(&b, SNX_AF_INET, type, 0)) ||
            (r = host_bind(a, &lo)) || (r = host_bind(b, &lo)) || (r = host_name(a, &la, 0)) ||
            (r = host_name(b, &lb, 0)) || (r = host_connect(a, &lb)) || (r = host_connect(b, &la)))
            goto fail;
    } else {
        if ((r = open_host(&l, SNX_AF_INET, type, 0)) || (r = host_bind(l, &lo)) || (r = host_listen(l, 1)) ||
            (r = host_name(l, &la, 0)) || (r = open_host(&a, SNX_AF_INET, type, 0)))
            goto fail;
        r = host_connect(a, &la); /* non-blocking: in progress until accepted */
        if (r && r != -SNX_EINPROGRESS) goto fail;
        if ((r = host_accept(l, &b, NULL, 0)) || (r = snx_net_wait(a->fd, POLLOUT, 0)) < 0) goto fail;
        snx_socket_unref(l);
    }
    a->domain = b->domain = domain;
    out[0] = a;
    out[1] = b;
    return 0;
fail:
    if (a) snx_socket_unref(a);
    if (b) snx_socket_unref(b);
    if (l) snx_socket_unref(l);
    return r;
#else
    int hd = snx_net_domain_to_host(domain), ht = snx_net_socktype_to_host(type), fd[2];
    if (hd < 0) return -SNX_EAFNOSUPPORT;
    if (ht < 0) return -SNX_ESOCKTNOSUPPORT;
    if (socketpair(hd, ht, protocol, fd)) return snx_net_host_error();
    prepare_host_fd(fd[0]);
    prepare_host_fd(fd[1]);
    out[0] = snx_net_socket_new(&g_host_ops, fd[0], domain, type, protocol);
    out[1] = snx_net_socket_new(&g_host_ops, fd[1], domain, type, protocol);
    if (!out[0] || !out[1]) {
        if (out[0])
            snx_socket_unref(out[0]);
        else
            close(fd[0]);
        if (out[1])
            snx_socket_unref(out[1]);
        else
            close(fd[1]);
        return -SNX_ENOMEM;
    }
    return 0;
#endif
}
int snx_socket_eventfd(SnxSocket** out, uint64_t initial, int semaphore) {
    int r = snx_net_init();
    return r ? r : snx_net_event_open(out, initial, semaphore);
}
SnxSocket* snx_socket_ref(SnxSocket* s) {
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
    return s;
}
void snx_socket_unref(SnxSocket* s) {
    if (!s || __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST)) return;
    if (s->ops->destroy) s->ops->destroy(s);
    free(s);
}
int snx_socket_domain(const SnxSocket* s) { return s->domain; }
int snx_socket_type(const SnxSocket* s) { return s->type; }
int snx_socket_protocol(const SnxSocket* s) { return s->protocol; }
int snx_socket_emulated(const SnxSocket* s) { return s->ops != &g_host_ops; }
int snx_socket_nonblocking(const SnxSocket* s) { return s->nonblocking; }
int snx_socket_set_nonblocking(SnxSocket* s, int on) {
    s->nonblocking = !!on;
    return 0;
}
#define SNX_NET_CALL(op, ...) (s->ops->op ? s->ops->op(__VA_ARGS__) : -SNX_EOPNOTSUPP)
int snx_socket_bind(SnxSocket* s, const SnxSockAddr* a) { return SNX_NET_CALL(bind, s, a); }
int snx_socket_connect(SnxSocket* s, const SnxSockAddr* a) { return SNX_NET_CALL(connect, s, a); }
int snx_socket_listen(SnxSocket* s, int backlog) { return SNX_NET_CALL(listen, s, backlog); }
int snx_socket_accept(SnxSocket* s, SnxSocket** out, SnxSockAddr* peer, int nonblocking) {
    return SNX_NET_CALL(accept, s, out, peer, nonblocking);
}
int snx_socket_shutdown(SnxSocket* s, int how) { return SNX_NET_CALL(shutdown, s, how); }
int64_t snx_socket_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to) {
    return SNX_NET_CALL(send, s, data, size, flags, to);
}
int64_t snx_socket_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags) {
    return SNX_NET_CALL(recv, s, data, size, flags, from, out_flags);
}
int snx_socket_name(SnxSocket* s, SnxSockAddr* out) { return SNX_NET_CALL(name, s, out, 0); }
int snx_socket_peer(SnxSocket* s, SnxSockAddr* out) { return SNX_NET_CALL(name, s, out, 1); }
int snx_socket_setsockopt(SnxSocket* s, int level, int name, const void* value, size_t size) {
    return SNX_NET_CALL(setsockopt, s, level, name, value, size);
}
int snx_socket_getsockopt(SnxSocket* s, int level, int name, void* value, size_t* size) {
    return SNX_NET_CALL(getsockopt, s, level, name, value, size);
}
int snx_socket_readable_bytes(SnxSocket* s, int* bytes) { return SNX_NET_CALL(readable_bytes, s, bytes); }

int snx_net_poll(SnxPollEntry* e, size_t count, int timeout_ms) {
    int r = snx_net_init();
    if (r) return r;
    HostPollFd stack[64];
    HostPollFd* f = count + 2 <= 64 ? stack : malloc((count + 2) * sizeof *f);
    size_t* idx = malloc((count + 1) * sizeof *idx);
    if (!f || !idx) {
        if (f != stack) free(f);
        free(idx);
        return -SNX_ENOMEM;
    }
    size_t n = 0;
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        e[i].revents = 0;
        SnxHostFd fd = SNX_BADFD;
        short hev = snx_net_poll_to_host(e[i].events);
        if (e[i].socket) {
            SnxSocket* s = e[i].socket;
            if (s->ops->poll_ready) e[i].revents = s->ops->poll_ready(s, e[i].events, &hev);
            fd = s->fd;
        }
#ifndef _WIN32
        else if (e[i].host_fd >= 0)
            fd = (SnxHostFd)e[i].host_fd;
#endif
        if (e[i].revents) ready = 1;
        if (fd == SNX_BADFD) continue;
        f[n] = (HostPollFd){0};
        f[n].fd = fd;
        f[n].events = hev;
        idx[n++] = i;
    }
    size_t cancel = n;
    if (g_cancel[0] != SNX_BADFD) {
        f[n] = (HostPollFd){0};
        f[n].fd = g_cancel[0];
        f[n++].events = POLLIN;
    }
    if (t_net.wake) {
        f[n] = (HostPollFd){0};
        f[n].fd = t_net.wake->fd[0];
        f[n++].events = POLLIN;
    }
    int64_t deadline = timeout_ms > 0 ? snx_net_now_us() + (int64_t)timeout_ms * 1000 : 0;
    for (;;) {
        int ms = ready || !timeout_ms ? 0 : timeout_ms < 0 ? -1 : ms_left(deadline);
        r = n ? host_poll(f, n, ms) : 0;
        if (!n && ms) snx_net_sleep_us(ms < 0 ? 1000000 : (int64_t)ms * 1000);
        if (interrupted(r)) continue;
        if (r < 0) {
            r = snx_net_host_error();
            break;
        }
        int hits = 0;
        for (size_t j = 0; j < cancel; j++)
            if (f[j].revents) e[idx[j]].revents |= snx_net_poll_from_host(f[j].revents);
        for (size_t i = 0; i < count; i++) hits += e[i].revents != 0;
        if (hits) {
            r = hits;
            break;
        }
        int stopped = 0;
        for (size_t j = cancel; j < n; j++) stopped |= f[j].revents != 0;
        if (stopped) {
            r = -SNX_EINTR;
            break;
        }
        if (!ms) {
            r = 0;
            break;
        }
    }
    if (f != stack) free(f);
    free(idx);
    return r;
}
