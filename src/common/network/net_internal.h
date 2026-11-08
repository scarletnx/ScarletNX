/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the stack's files share: the socket object, its operations, the host's socket API and small portability
   helpers (locks, threads, a pollable notifier) */
#ifndef SNX_NET_INTERNAL_H
#define SNX_NET_INTERNAL_H
#include "common/network/net.h"
#include "common/network/net_host.h"
#ifdef _WIN32
typedef SOCKET SnxHostFd;
typedef SRWLOCK SnxLock;
#define SNX_BADFD INVALID_SOCKET
#else
#include <pthread.h>
typedef int SnxHostFd;
typedef pthread_mutex_t SnxLock;
#define SNX_BADFD (-1)
#endif

/* What a socket kind does. A NULL entry fails with SNX_EOPNOTSUPP */
typedef struct SnxSocketOps {
    void (*destroy)(SnxSocket* s);
    int (*bind)(SnxSocket* s, const SnxSockAddr* addr);
    int (*connect)(SnxSocket* s, const SnxSockAddr* addr);
    int (*listen)(SnxSocket* s, int backlog);
    int (*accept)(SnxSocket* s, SnxSocket** out, SnxSockAddr* peer, int nonblocking);
    int (*shutdown)(SnxSocket* s, int how);
    int64_t (*send)(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to);
    int64_t (*recv)(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags);
    int (*name)(SnxSocket* s, SnxSockAddr* out, int peer);
    int (*setsockopt)(SnxSocket* s, int level, int name, const void* value, size_t size);
    int (*getsockopt)(SnxSocket* s, int level, int name, void* value, size_t* size);
    int (*readable_bytes)(SnxSocket* s, int* bytes);
    /* readiness the socket knows without asking the host (an emulated socket's write side), the events the host
       descriptor fd is polled for, and the translation of what the host reported */
    uint16_t (*poll_ready)(SnxSocket* s, uint16_t events, short* host_events);
} SnxSocketOps;

struct SnxSocket {
    const SnxSocketOps* ops;
    SnxHostFd fd; /* the host socket, or what an emulated socket polls */
    int domain, type, protocol;
    int nonblocking;
    int64_t rcvtimeo_us, sndtimeo_us; /* 0: wait forever */
    int refs;
    void* impl;
};

/* net.c */
SnxSocket* snx_net_socket_new(const SnxSocketOps* ops, SnxHostFd fd, int domain, int type, int protocol);
void snx_net_log(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void snx_net_warn(const char* fmt, ...) /* what the player must hear of */
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
int snx_net_host_error(void);       /* the last socket call's error, -SNX_E* */
int snx_net_close_fd(SnxHostFd fd);
int snx_net_set_host_nonblocking(SnxHostFd fd);
/* waits until fd is ready for host_events, timeout_us 0 = forever. 1 ready, -SNX_EAGAIN at the timeout,
   -SNX_EINTR when the stack is cancelled */
int snx_net_wait(SnxHostFd fd, short host_events, int64_t timeout_us);
int snx_net_wait_until(SnxHostFd fd, short host_events, int64_t deadline); /* deadline on snx_net_now_us, 0 none */
int64_t snx_net_deadline(int64_t timeout_us);                              /* 0 for timeout_us 0 */
/* the host socket's options, for emulated sockets that have a host socket. kept: the stack keeps the option itself
   (timeouts, the read-only ones), it needs no host socket */
int snx_net_host_setsockopt(SnxSocket* s, int level, int name, const void* value, size_t size);
int snx_net_host_getsockopt(SnxSocket* s, int level, int name, void* value, size_t* size);
int snx_net_sockopt_kept(int level, int name);
uint16_t snx_net_checksum(const void* data, size_t size, uint32_t sum); /* the Internet checksum */
int64_t snx_net_now_us(void);                                           /* a monotonic clock */
void snx_net_sleep_us(int64_t us);

/* locks, detached threads, and a pipe the stack can poll (a connected UDP pair on Windows) */
void snx_lock_init(SnxLock* l);
void snx_lock_destroy(SnxLock* l);
void snx_lock(SnxLock* l);
void snx_unlock(SnxLock* l);
int snx_net_thread(void (*fn)(void*), void* arg);
int snx_notifier_open(SnxHostFd out[2]); /* [0] is polled, [1] is written */
void snx_notifier_signal(SnxHostFd write_end);
void snx_notifier_drain(SnxHostFd read_end);

/* the emulated kinds */
int snx_net_icmp_open(SnxSocket** out, int domain, int type, int protocol); /* net_icmp.c */
int snx_net_event_open(SnxSocket** out, uint64_t initial, int semaphore);   /* net_event.c */

#endif
