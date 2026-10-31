/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/internal_network/socket_icmp.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
   The emulated ICMP socket: what a guest gets for a raw or datagram ICMP socket the host refuses (raw sockets need
   CAP_NET_RAW or root). It is only partly emulated: echo requests are the only ICMP it sends, and their replies
   the only ICMP it receives, which is what games and programs open these sockets for (pinging a host to see that
   the network works). Other ICMP messages are accepted and dropped.

   Three ways to get the echo answered, the first that works:
   - the host's unprivileged ping socket (SOCK_DGRAM + IPPROTO_ICMP: Linux within net.ipv4.ping_group_range, macOS).
     Real ICMP: the kernel sends the request and hands back the reply. It rewrites the echo identifier to the
     socket's, which the guest of a raw socket gets back as it sent it.
   - the host's ping program, once per request, the way Eden does it (Eden notes that opening a socket for this
     can get an address banned by some servers). A reply is made up when ping reports one.
   - Windows: IcmpSendEcho, which needs no privileges.
   A raw IPv4 socket receives the IP header before the ICMP message, so the stack builds one for the reply. Raw
   ICMPv6 sockets and datagram ICMP sockets receive the message alone.

   Eden's RecvFrom read the entry after the one it erased, and its ping wrote to the runtime's stdout. */
#include "common/network/net_internal.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

enum { BACKEND_PING_SOCKET, BACKEND_HELPER };
enum { MAX_PENDING = 32, MAX_QUEUED = 64, ID_RING = 64 };
#define ICMP_DEFAULT_WAIT_US 4000000
#define ICMP_MAX_WAIT_US     10000000

typedef struct Reply {
    struct Reply* next;
    SnxSockAddr from;
    size_t size;
    uint8_t data[]; /* the ICMP message, without an IP header */
} Reply;

typedef struct Icmp {
    SnxLock lock;
    int refs; /* the socket, and every request a helper still works on */
    int backend, closed, pending, queued;
    SnxHostFd notify[2]; /* the helper backend: readable while replies wait */
    Reply *head, *tail;
    int connected, bound;
    SnxSockAddr peer, local;
    uint16_t seq_ring[ID_RING], id_ring[ID_RING]; /* the ping socket backend: the guest's identifier per sequence */
    unsigned ring_next;
#ifndef _WIN32
    pid_t pids[MAX_PENDING];
#endif
} Icmp;

static void icmp_unref(Icmp* m) {
    snx_lock(&m->lock);
    int left = --m->refs;
    snx_unlock(&m->lock);
    if (left) return;
    for (Reply* r = m->head; r;) {
        Reply* n = r->next;
        free(r);
        r = n;
    }
    if (m->notify[0] != SNX_BADFD) snx_net_close_fd(m->notify[0]);
    if (m->notify[1] != SNX_BADFD) snx_net_close_fd(m->notify[1]);
    snx_lock_destroy(&m->lock);
    free(m);
}

static int is_v6(const SnxSocket* s) { return s->domain == SNX_AF_INET6; }
static uint8_t echo_request(const SnxSocket* s) { return is_v6(s) ? 128 : 8; }
static uint8_t echo_reply(const SnxSocket* s) { return is_v6(s) ? 129 : 0; }

/* ------------------------------------------------------------ helper backend: replies queued by worker threads */
static void queue_reply(Icmp* m, const SnxSockAddr* from, const uint8_t* msg, size_t size) {
    Reply* r = malloc(sizeof *r + size);
    if (!r) return;
    r->next = NULL;
    r->from = *from;
    r->size = size;
    memcpy(r->data, msg, size);
    snx_lock(&m->lock);
    if (m->closed || m->queued >= MAX_QUEUED) {
        snx_unlock(&m->lock);
        free(r);
        return;
    }
    if (m->tail)
        m->tail->next = r;
    else
        m->head = r;
    m->tail = r;
    if (m->queued++ == 0) snx_notifier_signal(m->notify[1]);
    snx_unlock(&m->lock);
}
/* The caller owns the returned reply, including when peeking: another receiver can dequeue it immediately. */
static int pop_reply(Icmp* m, int peek, Reply** out) {
    snx_lock(&m->lock);
    Reply* r = m->head;
    int error = 0;
    if (r && peek) {
        Reply* copy = malloc(sizeof *r + r->size);
        if (copy) {
            memcpy(copy, r, sizeof *r + r->size);
            copy->next = NULL;
        } else
            error = -SNX_ENOMEM;
        r = copy;
    } else if (r) {
        m->head = r->next;
        if (!m->head) m->tail = NULL;
        if (--m->queued == 0) snx_notifier_drain(m->notify[0]);
    }
    snx_unlock(&m->lock);
    *out = r;
    return error;
}

typedef struct Request {
    Icmp* m;
    int v6, slot;
    int64_t wait_us;
    SnxSockAddr to;
    size_t size;
    uint8_t msg[]; /* the guest's echo request */
} Request;

#ifdef _WIN32
static int run_echo(Request* q, uint8_t** data, size_t* size) {
    if (q->v6) return 0;
    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return 0;
    size_t payload = q->size - 8;
    DWORD cap = (DWORD)(sizeof(ICMP_ECHO_REPLY) + payload + 8 + 64);
    void* buf = malloc(cap);
    IPAddr ip;
    memcpy(&ip, q->to.addr, 4);
    DWORD n = buf ? IcmpSendEcho(h, ip, q->msg + 8, (WORD)payload, NULL, buf, cap, (DWORD)(q->wait_us / 1000)) : 0;
    int ok = 0;
    if (n) {
        ICMP_ECHO_REPLY* r = buf;
        if (r->Status == IP_SUCCESS && (*data = malloc(r->DataSize + 1))) {
            memcpy(*data, r->Data, r->DataSize);
            *size = r->DataSize;
            ok = 1;
        }
    }
    free(buf);
    IcmpCloseHandle(h);
    return ok;
}
#elif defined(__ANDROID__) && __ANDROID_API__ < 28
/* posix_spawn comes with Android 9, and the app runs on older ones: there echoes have the ping socket alone */
static int run_echo(Request* q, uint8_t** data, size_t* size) {
    (void)q;
    (void)data;
    (void)size;
    return 0;
}
#else
static int run_echo(Request* q, uint8_t** data, size_t* size) {
    char addr[64], wait[32];
    if (!inet_ntop(q->v6 ? AF_INET6 : AF_INET, q->to.addr, addr, sizeof addr)) return 0;
#if defined(__linux__)
    /* iputils and BusyBox take whole seconds */
    snprintf(wait, sizeof wait, "%lld", (long long)((q->wait_us + 999999) / 1000000));
    const char* argv[] = {"ping", q->v6 ? "-6" : "-4", "-n", "-q", "-c", "1", "-W", wait, addr, NULL};
#else
    /* the BSDs and macOS take milliseconds, and ping6 for IPv6 */
    snprintf(wait, sizeof wait, "%lld", (long long)(q->wait_us / 1000));
    const char* argv[] = {q->v6 ? "ping6" : "ping", "-n", "-q", "-c", "1", "-W", wait, addr, NULL};
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r) {
        static int warned;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
            snx_net_log("net: emulated ICMP has neither a ping socket nor a ping program (%s), echoes go unanswered",
                        strerror(r));
        return 0;
    }
    snx_lock(&q->m->lock);
    q->m->pids[q->slot] = pid;
    if (q->m->closed) kill(pid, SIGKILL); /* close may have raced with spawn */
    snx_unlock(&q->m->lock);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    snx_lock(&q->m->lock);
    /* Keep the slot reserved until echo_worker releases it, even after the child exits. */
    q->m->pids[q->slot] = -1;
    snx_unlock(&q->m->lock);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 0;
    /* ping answered: the reply carries what the request did */
    *data = malloc(q->size - 8 + 1);
    if (!*data) return 0;
    memcpy(*data, q->msg + 8, q->size - 8);
    *size = q->size - 8;
    return 1;
}
#endif
static void echo_worker(void* p) {
    Request* q = p;
    uint8_t* payload = NULL;
    size_t n = 0;
    if (run_echo(q, &payload, &n)) {
        uint8_t* msg = malloc(8 + n);
        if (msg) {
            memcpy(msg, q->msg, 8); /* type, code, checksum, identifier and sequence as the guest sent them */
            msg[0] = q->v6 ? 129 : 0;
            msg[2] = msg[3] = 0;
            memcpy(msg + 8, payload, n);
            if (!q->v6) { /* ICMPv6's checksum covers a pseudo-header the guest's kernel would check, not the guest */
                uint16_t c = snx_net_checksum(msg, 8 + n, 0);
                snx_st_be16(msg + 2, c);
            }
            queue_reply(q->m, &q->to, msg, 8 + n);
            free(msg);
        }
    }
    free(payload);
    snx_lock(&q->m->lock);
#ifndef _WIN32
    q->m->pids[q->slot] = 0; /* also release the reservation when spawn failed */
#endif
    q->m->pending--;
    snx_unlock(&q->m->lock);
    icmp_unref(q->m);
    free(q);
}

/* ------------------------------------------------------------ the socket */
static void icmp_destroy(SnxSocket* s) {
    Icmp* m = s->impl;
    if (s->fd != SNX_BADFD && m->backend == BACKEND_PING_SOCKET) snx_net_close_fd(s->fd);
    snx_lock(&m->lock);
    m->closed = 1;
#ifndef _WIN32
    for (int i = 0; i < MAX_PENDING; i++)
        if (m->pids[i] > 0) kill(m->pids[i], SIGKILL);
#endif
    snx_unlock(&m->lock);
    icmp_unref(m);
}
static int icmp_bind(SnxSocket* s, const SnxSockAddr* a) {
    Icmp* m = s->impl;
    if (a->family != s->domain) return -SNX_EAFNOSUPPORT;
    if (m->backend == BACKEND_PING_SOCKET) {
        struct sockaddr_storage ss;
        snx_socklen n;
        SnxSockAddr b = *a;
        b.port = 0; /* a ping socket's port is its echo identifier, a raw socket has none */
        int r = snx_net_addr_to_host(&b, &ss, &n);
        if (r) return r;
        if (bind(s->fd, (struct sockaddr*)&ss, n)) return snx_net_host_error();
    }
    m->local = *a;
    m->bound = 1;
    return 0;
}
static int icmp_connect(SnxSocket* s, const SnxSockAddr* a) {
    Icmp* m = s->impl;
    if (a->family == SNX_AF_UNSPEC) {
        m->connected = 0;
        return 0;
    }
    if (a->family != s->domain) return -SNX_EAFNOSUPPORT;
    m->peer = *a;
    m->connected = 1;
    return 0;
}
static int icmp_shutdown(SnxSocket* s, int how) {
    (void)s;
    return how < SNX_SHUT_RD || how > SNX_SHUT_RDWR ? -SNX_EINVAL : -SNX_ENOTCONN;
}
static int64_t icmp_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to) {
    Icmp* m = s->impl;
    const uint8_t* msg = data;
    (void)flags;
    if (!to) {
        if (!m->connected) return -SNX_EDESTADDRREQ;
        to = &m->peer;
    }
    if (to->family != s->domain) return -SNX_EAFNOSUPPORT;
    if (size < 8) return -SNX_EINVAL;
    if (size > 65507) return -SNX_EMSGSIZE;
    if (msg[0] != echo_request(s) || msg[1]) {
        static int warned;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
            snx_net_log("net: emulated ICMP sends echo requests only, dropping type %u", msg[0]);
        return (int64_t)size;
    }
    if (m->backend == BACKEND_PING_SOCKET) {
        snx_lock(&m->lock);
        unsigned i = m->ring_next++ % ID_RING;
        m->seq_ring[i] = snx_ld_be16(msg + 6);
        m->id_ring[i] = snx_ld_be16(msg + 4);
        snx_unlock(&m->lock);
        struct sockaddr_storage ss;
        snx_socklen n;
        SnxSockAddr dst = *to;
        dst.port = 0;
        int r = snx_net_addr_to_host(&dst, &ss, &n);
        if (r) return r;
        for (;;) {
#ifdef _WIN32
            int sent = sendto(s->fd, (const char*)data, (int)size, 0, (struct sockaddr*)&ss, n);
#else
            ssize_t sent = sendto(s->fd, data, size, 0, (struct sockaddr*)&ss, n);
#endif
            if (sent >= 0) return sent;
            r = snx_net_host_error();
            if (r != -SNX_EAGAIN || s->nonblocking || (flags & SNX_MSG_DONTWAIT)) return r;
            r = snx_net_wait(s->fd, POLLOUT, s->sndtimeo_us);
            if (r < 0) return r;
        }
    }
    Request* q = malloc(sizeof *q + size);
    if (!q) return -SNX_ENOMEM;
    q->m = m;
    q->v6 = is_v6(s);
    q->to = *to;
    q->size = size;
    q->wait_us = s->rcvtimeo_us > 0 && s->rcvtimeo_us < ICMP_MAX_WAIT_US ? s->rcvtimeo_us : ICMP_DEFAULT_WAIT_US;
    memcpy(q->msg, data, size);
    snx_lock(&m->lock);
    if (m->pending >= MAX_PENDING) {
        snx_unlock(&m->lock);
        free(q);
        return -SNX_ENOBUFS;
    }
    q->slot = 0;
#ifndef _WIN32
    /* a free slot for the ping's pid, so closing the socket can stop it */
    for (int i = 0; i < MAX_PENDING; i++)
        if (!m->pids[i]) {
            q->slot = i;
            m->pids[i] = -1;
            break;
        }
#endif
    m->pending++;
    m->refs++;
    snx_unlock(&m->lock);
    if (snx_net_thread(echo_worker, q)) {
        snx_lock(&m->lock);
        m->pending--;
        m->refs--;
#ifndef _WIN32
        m->pids[q->slot] = 0;
#endif
        snx_unlock(&m->lock);
        free(q);
        return -SNX_ENOBUFS;
    }
    return (int64_t)size;
}
/* where the reply says it went: the bound address, or the host's for a reply from elsewhere */
static void local_address(Icmp* m, const SnxSockAddr* from, uint8_t out[4]) {
    if (m->bound && memcmp(m->local.addr, "\0\0\0\0", 4)) {
        memcpy(out, m->local.addr, 4);
        return;
    }
    SnxNetInterface i;
    if (from->addr[0] != 127 && !snx_net_primary_interface(&i))
        memcpy(out, i.ip, 4);
    else
        memcpy(out, (const uint8_t[]){127, 0, 0, 1}, 4);
}
/* hands msg to the guest: behind an IPv4 header for a raw IPv4 socket */
static int64_t deliver(SnxSocket* s, const uint8_t* msg, size_t n, const SnxSockAddr* src, uint8_t* out, size_t size,
                       int* out_flags) {
    Icmp* m = s->impl;
    uint8_t ip[20];
    size_t head = 0;
    if (s->type == SNX_SOCK_RAW && !is_v6(s)) {
        size_t total = 20 + n;
        memset(ip, 0, sizeof ip);
        ip[0] = 0x45;
        snx_st_be16(ip + 2, (uint16_t)total);
        ip[8] = 64; /* TTL */
        ip[9] = SNX_IPPROTO_ICMP;
        memcpy(ip + 12, src->addr, 4);
        local_address(m, src, ip + 16);
        uint16_t c = snx_net_checksum(ip, 20, 0);
        snx_st_be16(ip + 10, c);
        head = 20;
    }
    size_t want = head + n, done = 0;
    if (head) {
        done = head < size ? head : size;
        memcpy(out, ip, done);
    }
    if (size > done) {
        size_t k = n < size - done ? n : size - done;
        memcpy(out + done, msg, k);
        done += k;
    }
    if (done < want && out_flags) *out_flags |= SNX_MSG_TRUNC;
    return (int64_t)done;
}
static int64_t icmp_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags) {
    Icmp* m = s->impl;
    int dontwait = s->nonblocking || (flags & SNX_MSG_DONTWAIT);
    int64_t deadline = snx_net_deadline(s->rcvtimeo_us);
    if (out_flags) *out_flags = 0;
    for (;;) {
        if (m->backend == BACKEND_PING_SOCKET) {
            uint8_t buf[65536];
            struct sockaddr_storage ss;
            snx_socklen sl = sizeof ss;
            int64_t r = recvfrom(s->fd, (char*)buf, sizeof buf, (flags & SNX_MSG_PEEK) ? MSG_PEEK : 0,
                                 (struct sockaddr*)&ss, &sl);
            if (r >= 0) {
                uint8_t* msg = buf;
                size_t n = (size_t)r;
                /* macOS hands datagram ICMP sockets the IP header too */
                if (!is_v6(s) && n >= 20 && (msg[0] >> 4) == 4 && msg[9] == SNX_IPPROTO_ICMP) {
                    size_t hl = (size_t)(msg[0] & 15) * 4;
                    if (hl <= n) msg += hl, n -= hl;
                }
                SnxSockAddr src;
                if (snx_net_addr_from_host((struct sockaddr*)&ss, sl, &src)) memset(&src, 0, sizeof src);
                src.port = 0;
                if (n >= 8 && msg[0] == echo_reply(s)) {
                    /* an echo of this socket's, by its sequence: macOS hands a datagram ICMP socket every echo reply
                       of the system's, another program's too, which goes (taken off the queue when peeked) */
                    uint16_t seq = snx_ld_be16(msg + 6), id = 0;
                    int ours = 0;
                    snx_lock(&m->lock);
                    unsigned count = m->ring_next < ID_RING ? m->ring_next : ID_RING;
                    for (unsigned k = 0; k < count && !ours; k++) {
                        unsigned i = (m->ring_next - 1 - k) % ID_RING;
                        if (m->seq_ring[i] == seq) {
                            id = m->id_ring[i];
                            ours = 1;
                        }
                    }
                    snx_unlock(&m->lock);
                    if (!ours) {
                        if (flags & SNX_MSG_PEEK) (void)recvfrom(s->fd, (char*)buf, sizeof buf, 0, NULL, NULL);
                        continue;
                    }
                    /* the kernel put the socket's identifier in the echo: the raw socket's guest wants its own */
                    if (s->type == SNX_SOCK_RAW) {
                        snx_st_be16(msg + 4, id);
                        if (!is_v6(s)) {
                            msg[2] = msg[3] = 0;
                            uint16_t c = snx_net_checksum(msg, n, 0);
                            snx_st_be16(msg + 2, c);
                        }
                    }
                }
                if (from) *from = src;
                return deliver(s, msg, n, &src, data, size, out_flags);
            }
            if (r != -1 || (r = snx_net_host_error()) != -SNX_EAGAIN || dontwait) return r;
        } else {
            Reply* r;
            int error = pop_reply(m, flags & SNX_MSG_PEEK, &r);
            if (error) return error;
            if (r) {
                if (from) *from = r->from;
                int64_t n = deliver(s, r->data, r->size, &r->from, data, size, out_flags);
                free(r);
                return n;
            }
            if (dontwait) return -SNX_EAGAIN;
        }
        int w = snx_net_wait_until(s->fd, POLLIN, deadline);
        if (w < 0) return w;
    }
}
static int icmp_name(SnxSocket* s, SnxSockAddr* out, int peer) {
    Icmp* m = s->impl;
    if (peer) {
        if (!m->connected) return -SNX_ENOTCONN;
        *out = m->peer;
        return 0;
    }
    if (m->bound)
        *out = m->local;
    else {
        memset(out, 0, sizeof *out);
        out->family = (uint16_t)s->domain;
    }
    out->port = 0;
    return 0;
}
static int icmp_setsockopt(SnxSocket* s, int level, int name, const void* v, size_t size) {
    Icmp* m = s->impl;
    if (snx_net_sockopt_kept(level, name)) return snx_net_host_setsockopt(s, level, name, v, size);
    if (m->backend == BACKEND_PING_SOCKET) {
        if (level == SNX_SOL_IP && name == SNX_IP_HDRINCL) return 0; /* the guest's header never reaches the host */
        int r = snx_net_host_setsockopt(s, level, name, v, size);
        return r == -SNX_ENOPROTOOPT || r == -SNX_EINVAL || r == -SNX_EOPNOTSUPP ? 0 : r;
    }
    return 0; /* TTL, TOS, buffer sizes: nothing of the helper's to change */
}
static int icmp_getsockopt(SnxSocket* s, int level, int name, void* v, size_t* size) {
    Icmp* m = s->impl;
    if (snx_net_sockopt_kept(level, name) || (m->backend == BACKEND_PING_SOCKET && level != SNX_SOL_IP))
        return snx_net_host_getsockopt(s, level, name, v, size);
    int32_t zero = 0;
    size_t n = *size < 4 ? *size : 4;
    memcpy(v, &zero, n);
    *size = n;
    return 0;
}
static int icmp_readable_bytes(SnxSocket* s, int* bytes) {
    Icmp* m = s->impl;
    *bytes = 0;
    if (m->backend == BACKEND_PING_SOCKET) {
#ifdef _WIN32
        return 0;
#else
        uint8_t buf[1];
        ssize_t r = recv(s->fd, buf, sizeof buf, MSG_PEEK | MSG_TRUNC);
        if (r > 0) *bytes = (int)r + (s->type == SNX_SOCK_RAW && !is_v6(s) ? 20 : 0);
        return 0;
#endif
    }
    snx_lock(&m->lock);
    if (m->head) *bytes = (int)m->head->size + (s->type == SNX_SOCK_RAW && !is_v6(s) ? 20 : 0);
    snx_unlock(&m->lock);
    return 0;
}
static uint16_t icmp_poll_ready(SnxSocket* s, uint16_t events, short* host_events) {
    Icmp* m = s->impl;
    if (m->backend == BACKEND_PING_SOCKET) return 0;
    *host_events = (events & (SNX_POLLIN | SNX_POLLRDNORM)) ? POLLIN : 0;
    return events & (SNX_POLLOUT | SNX_POLLWRNORM); /* a request never waits for room */
}
static const SnxSocketOps g_icmp_ops = {.destroy = icmp_destroy,
                                        .bind = icmp_bind,
                                        .connect = icmp_connect,
                                        .shutdown = icmp_shutdown,
                                        .send = icmp_send,
                                        .recv = icmp_recv,
                                        .name = icmp_name,
                                        .setsockopt = icmp_setsockopt,
                                        .getsockopt = icmp_getsockopt,
                                        .readable_bytes = icmp_readable_bytes,
                                        .poll_ready = icmp_poll_ready};

int snx_net_icmp_open(SnxSocket** out, int domain, int type, int protocol) {
    Icmp* m = calloc(1, sizeof *m);
    if (!m) return -SNX_ENOMEM;
    snx_lock_init(&m->lock);
    m->refs = 1;
    m->notify[0] = m->notify[1] = SNX_BADFD;
    SnxHostFd fd = SNX_BADFD;
#ifndef _WIN32
    int hd = snx_net_domain_to_host(domain);
#ifdef SOCK_CLOEXEC
    fd = socket(hd, SOCK_DGRAM | SOCK_CLOEXEC, protocol);
#else
    fd = socket(hd, SOCK_DGRAM, protocol);
#endif
#endif
    if (fd != SNX_BADFD) {
        snx_net_set_host_nonblocking(fd);
        m->backend = BACKEND_PING_SOCKET;
    } else {
        int r = snx_notifier_open(m->notify);
        if (r) {
            icmp_unref(m);
            return r;
        }
        m->backend = BACKEND_HELPER;
        fd = m->notify[0];
#ifdef _WIN32
        if (domain == SNX_AF_INET6) {
            icmp_unref(m);
            return -SNX_EAFNOSUPPORT;
        }
#endif
    }
    SnxSocket* s = snx_net_socket_new(&g_icmp_ops, fd, domain, type, protocol);
    if (!s) {
        if (m->backend == BACKEND_PING_SOCKET) snx_net_close_fd(fd);
        icmp_unref(m);
        return -SNX_ENOMEM;
    }
    s->impl = m;
    static int told;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
        snx_net_log("net: the host refused an ICMP socket, emulating it with %s",
                    m->backend == BACKEND_PING_SOCKET ? "a ping socket" : "the host's ping");
    *out = s;
    return 0;
}
