/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's socket syscalls and eventfd2. Linux's layouts (AArch64): sockaddr_in {u16 family, be16 port, in_addr,
   zero[8]}, sockaddr_in6 {u16 family, be16 port, u32 flowinfo, in6_addr, u32 scope_id}, msghdr {name, u32 namelen,
   iov, size_t iovlen, control, size_t controllen, int flags} (56 bytes), mmsghdr {msghdr, u32 len} (64 bytes).
   AF_UNIX sockets serve socketpair() only: a path would name a host file, and the guest's filesystem is its own.
   Control messages aren't passed: recvmsg() reports none. */
#include "common/utilities/endian.h"
#include "gnuabi/network/network.h"
#include "gnuabi/filesystem/internal.h"
#include "gnuabi/abi/linux_net.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(SNX_EAGAIN == GNU_EAGAIN && SNX_EINVAL == GNU_EINVAL && SNX_ETIMEDOUT == GNU_ETIMEDOUT,
               "the stack's errno is the guest's");

enum {
    LINUX_SOCK_NONBLOCK = 0x800,
    LINUX_SOCK_CLOEXEC = 0x80000,
    LINUX_SOCK_TYPE_MASK = 0xF,
    LINUX_EFD_SEMAPHORE = 1,
    LINUX_MSG_DONTWAIT = 0x40,
    LINUX_MSG_WAITFORONE = 0x10000,
    MSGHDR_SIZE = 56,
    MMSGHDR_SIZE = 64,
    BOUNCE_MAX = 64 * 1024 * 1024,
};

/* ============================================================ translations (abi/linux_net.h) */
uint16_t gnu_net_poll_in(uint32_t e) { return gnu_linux_poll_in(e); }
uint32_t gnu_net_poll_out(uint16_t e) { return gnu_linux_poll_out(e); }
/* ============================================================ addresses */
/* 0 and the address, 1 for an AF_UNIX one, or -errno */
static int addr_in(GnuMemory* m, uint64_t a, uint64_t len, SnxSockAddr* out) {
    unsigned char b[GNU_LINUX_SOCKADDR_MAX] = {0};
    *out = (SnxSockAddr){0};
    if (!a || len < 2 || len > 128) return -GNU_EINVAL;
    int e = gnu_memory_read(m, a, b, len < sizeof b ? (size_t)len : sizeof b);
    return e ? e : gnu_linux_addr_decode(b, len, out);
}
/* the address into (addr, *lenp): what fits of it, and its whole size in *lenp */
static int addr_out(GnuMemory* m, const SnxSockAddr* sa, uint64_t addr, uint64_t lenp) {
    if (!addr || !lenp) return 0;
    unsigned char b[GNU_LINUX_SOCKADDR_MAX], l[4];
    int e = gnu_memory_read(m, lenp, l, 4);
    if (e) return e;
    uint32_t cap = snx_ld_le32(l);
    if ((int32_t)cap < 0) return -GNU_EINVAL;
    uint32_t n = gnu_linux_addr_encode(sa, b);
    if ((e = gnu_memory_write(m, addr, b, cap < n ? cap : n))) return e;
    gnu_vfs_put32(l, n);
    return gnu_memory_write(m, lenp, l, 4);
}

/* ============================================================ the descriptors */
static int64_t sock_read(void* o, unsigned flags, void* data, size_t size) {
    snx_socket_set_nonblocking(o, (flags & GNU_O_NONBLOCK) != 0);
    return snx_socket_recv(o, data, size, 0, NULL, NULL);
}
static int64_t sock_write(void* o, unsigned flags, const void* data, size_t size) {
    snx_socket_set_nonblocking(o, (flags & GNU_O_NONBLOCK) != 0);
    return snx_socket_send(o, data, size, 0, NULL);
}
static int sock_readable(void* o, int* bytes) { return snx_socket_readable_bytes(o, bytes); }
static void sock_release(void* o) { snx_socket_unref(o); }
static const GnuObjectOps g_socket_ops = {"socket", 0140000, sock_read, sock_write, sock_readable, sock_release};
static const GnuObjectOps g_eventfd_ops = {"anon_inode:[eventfd]", 0,           sock_read, sock_write,
                                           sock_readable,          sock_release};

SnxSocket* gnu_net_pollable(const GnuFdInfo* i) {
    return i->ops == &g_socket_ops || i->ops == &g_eventfd_ops ? i->object : NULL;
}
static SnxSocket* sock_of(GnuProcess* p, uint64_t fd, int64_t* err) {
    unsigned flags = 0;
    SnxSocket* s = gnu_vfs_object(&p->vfs, (int)fd, &g_socket_ops, &flags);
    if (!s) {
        GnuFdInfo i;
        *err = gnu_vfs_fd_info(&p->vfs, (int)fd, &i) ? -GNU_EBADF : -SNX_ENOTSOCK;
        return NULL;
    }
    snx_socket_set_nonblocking(s, (flags & GNU_O_NONBLOCK) != 0);
    return s;
}
#define SOCK_OR_FAIL(fd)                                                                                               \
    int64_t err_;                                                                                                      \
    SnxSocket* s = sock_of(p, fd, &err_);                                                                              \
    if (!s) return err_

static int64_t install(GnuProcess* p, SnxSocket* s, uint64_t typeflags) {
    return gnu_vfs_install_object(&p->vfs, &g_socket_ops, s, (typeflags & LINUX_SOCK_NONBLOCK) ? GNU_O_NONBLOCK : 0,
                                  (typeflags & LINUX_SOCK_CLOEXEC) != 0);
}
static int64_t sys_socket(GnuProcess* p, const uint64_t a[6]) {
    int dom = gnu_linux_domain_in((uint32_t)a[0]), type = (int)(a[1] & LINUX_SOCK_TYPE_MASK);
    if (a[1] & ~(uint64_t)(LINUX_SOCK_TYPE_MASK | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) return -GNU_EINVAL;
    if (dom < 0 || dom == SNX_AF_UNSPEC) return -SNX_EAFNOSUPPORT;
    SnxSocket* s;
    int r = snx_socket_open(&s, dom, type, (int)a[2]);
    return r ? r : install(p, s, a[1]);
}
static int64_t sys_socketpair(GnuProcess* p, const uint64_t a[6]) {
    int dom = gnu_linux_domain_in((uint32_t)a[0]), type = (int)(a[1] & LINUX_SOCK_TYPE_MASK);
    if (a[1] & ~(uint64_t)(LINUX_SOCK_TYPE_MASK | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) return -GNU_EINVAL;
    if (dom != SNX_AF_UNIX) return dom < 0 ? -SNX_EAFNOSUPPORT : -SNX_EOPNOTSUPP;
    if (!gnu_memory_access(&p->memory, a[3], 8, GNU_PROT_WRITE)) return -GNU_EFAULT;
    SnxSocket* s[2];
    int r = snx_socket_pair(s, dom, type, (int)a[2]);
    if (r) return r;
    int64_t f0 = install(p, s[0], a[1]);
    if (f0 < 0) {
        snx_socket_unref(s[1]);
        return f0;
    }
    int64_t f1 = install(p, s[1], a[1]);
    if (f1 < 0) {
        const uint64_t close_args[6] = {(uint64_t)f0};
        int64_t ignored;
        gnu_vfs_syscall(&p->vfs, &p->memory, 57, close_args, &ignored); /* close() */
        return f1;
    }
    unsigned char fds[8];
    gnu_vfs_put32(fds, (uint32_t)f0);
    gnu_vfs_put32(fds + 4, (uint32_t)f1);
    return gnu_memory_write(&p->memory, a[3], fds, 8);
}
static int64_t sys_bind_connect(GnuProcess* p, const uint64_t a[6], int connect) {
    SOCK_OR_FAIL(a[0]);
    SnxSockAddr sa;
    int r = addr_in(&p->memory, a[1], a[2], &sa);
    if (r == 1) return connect ? -GNU_ENOENT : -SNX_EADDRNOTAVAIL; /* no path reaches the guest's filesystem */
    if (r) return r;
    return connect ? snx_socket_connect(s, &sa) : snx_socket_bind(s, &sa);
}
static int64_t sys_accept4(GnuProcess* p, const uint64_t a[6], uint64_t flags) {
    if (flags & ~(uint64_t)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) return -GNU_EINVAL;
    SOCK_OR_FAIL(a[0]);
    SnxSocket* n;
    SnxSockAddr peer;
    int r = snx_socket_accept(s, &n, &peer, (flags & LINUX_SOCK_NONBLOCK) != 0); /* Linux doesn't inherit it */
    if (r) return r;
    int64_t fd = install(p, n, flags);
    if (fd >= 0 && (r = addr_out(&p->memory, &peer, a[1], a[2]))) return r;
    return fd;
}
static int64_t sys_name(GnuProcess* p, const uint64_t a[6], int peer) {
    SOCK_OR_FAIL(a[0]);
    if (!a[1] || !a[2]) return -GNU_EFAULT;
    SnxSockAddr sa;
    int r = peer ? snx_socket_peer(s, &sa) : snx_socket_name(s, &sa);
    return r ? r : addr_out(&p->memory, &sa, a[1], a[2]);
}
static int64_t sys_sendto(GnuProcess* p, const uint64_t a[6]) {
    SOCK_OR_FAIL(a[0]);
    SnxSockAddr to;
    if (a[4]) {
        int r = addr_in(&p->memory, a[4], a[5], &to);
        if (r) return r == 1 ? -GNU_ENOENT : r;
    }
    void* data = gnu_memory_access(&p->memory, a[1], (size_t)a[2], GNU_PROT_READ);
    if (!data) return -GNU_EFAULT;
    return snx_socket_send(s, data, (size_t)a[2], gnu_linux_msg_in(a[3]), a[4] ? &to : NULL);
}
static int64_t sys_recvfrom(GnuProcess* p, const uint64_t a[6]) {
    SOCK_OR_FAIL(a[0]);
    void* data = gnu_memory_access(&p->memory, a[1], (size_t)a[2], GNU_PROT_WRITE);
    if (!data) return -GNU_EFAULT;
    SnxSockAddr from;
    int64_t r = snx_socket_recv(s, data, (size_t)a[2], gnu_linux_msg_in(a[3]), a[4] ? &from : NULL, NULL);
    if (r >= 0 && a[4]) {
        int e = addr_out(&p->memory, &from, a[4], a[5]);
        if (e) return e;
    }
    return r;
}
static int64_t sys_setsockopt(GnuProcess* p, const uint64_t a[6]) {
    SOCK_OR_FAIL(a[0]);
    int level, name, layout, r = gnu_linux_sockopt_in(a[1], a[2], &level, &name, &layout);
    if (r) return r;
    unsigned char v[64] = {0};
    size_t len = a[4] < sizeof v ? (size_t)a[4] : sizeof v;
    if ((int32_t)a[4] < 0) return -GNU_EINVAL;
    if (len && (r = gnu_memory_read(&p->memory, a[3], v, len))) return r;
    if (layout == GNU_LAYOUT_MREQN && len > sizeof(SnxIpMreq)) len = sizeof(SnxIpMreq); /* ip_mreqn: its ifindex goes */
    if (layout == GNU_LAYOUT_IFADDR && len >= 8) {                                      /* ip_mreq(n): the address */
        memmove(v, v + 4, 4);
        len = 4;
    }
    return snx_socket_setsockopt(s, level, name, v, len);
}
static int64_t sys_getsockopt(GnuProcess* p, const uint64_t a[6]) {
    SOCK_OR_FAIL(a[0]);
    int level, name, layout, r = gnu_linux_sockopt_in(a[1], a[2], &level, &name, &layout);
    if (r) return r;
    unsigned char l[4], v[64] = {0};
    if ((r = gnu_memory_read(&p->memory, a[4], l, 4))) return r;
    int32_t cap = (int32_t)(l[0] | l[1] << 8 | l[2] << 16 | (uint32_t)l[3] << 24);
    if (cap < 0) return -GNU_EINVAL;
    size_t len = sizeof v;
    if ((r = snx_socket_getsockopt(s, level, name, v, &len))) return r;
    if ((size_t)cap < len) len = (size_t)cap;
    if (len && (r = gnu_memory_write(&p->memory, a[3], v, len))) return r;
    gnu_vfs_put32(l, (uint32_t)len);
    return gnu_memory_write(&p->memory, a[4], l, 4);
}

/* msghdr's fields, and its vectors gathered into (or scattered from) one buffer */
typedef struct {
    uint64_t name, iov, iovlen, control;
    uint32_t namelen;
} MsgHdr;
static int read_msghdr(GnuMemory* m, uint64_t a, MsgHdr* h) {
    unsigned char b[MSGHDR_SIZE];
    int e = gnu_memory_read(m, a, b, sizeof b);
    if (e) return e;
#define GET64(o)                                                                                                       \
    ((uint64_t)b[o] | (uint64_t)b[o + 1] << 8 | (uint64_t)b[o + 2] << 16 | (uint64_t)b[o + 3] << 24 |                  \
     (uint64_t)b[o + 4] << 32 | (uint64_t)b[o + 5] << 40 | (uint64_t)b[o + 6] << 48 | (uint64_t)b[o + 7] << 56)
    h->name = GET64(0);
    h->namelen = (uint32_t)GET64(8);
    h->iov = GET64(16);
    h->iovlen = GET64(24);
    h->control = GET64(32);
    return h->iovlen > 1024 ? -SNX_EMSGSIZE : 0;
}
static int64_t iov_total(GnuMemory* m, const MsgHdr* h, unsigned char* vec) {
    if (h->iovlen && gnu_memory_read(m, h->iov, vec, (size_t)h->iovlen * 16)) return -GNU_EFAULT;
    uint64_t total = 0;
    for (uint64_t i = 0; i < h->iovlen; i++) {
        uint64_t n = 0;
        for (int k = 0; k < 8; k++) n |= (uint64_t)vec[i * 16 + 8 + k] << (k * 8);
        if (n > BOUNCE_MAX - total) return -GNU_EINVAL;
        total += n;
    }
    return (int64_t)total;
}
static int copy_vectors(GnuMemory* m, const MsgHdr* h, const unsigned char* vec, unsigned char* buf, size_t size,
                        int gather) {
    size_t at = 0;
    for (uint64_t i = 0; i < h->iovlen && at < size; i++) {
        uint64_t base = 0, n = 0;
        for (int k = 0; k < 8; k++) {
            base |= (uint64_t)vec[i * 16 + k] << (k * 8);
            n |= (uint64_t)vec[i * 16 + 8 + k] << (k * 8);
        }
        if (n > size - at) n = size - at;
        int e = gather ? gnu_memory_read(m, base, buf + at, (size_t)n) : gnu_memory_write(m, base, buf + at, (size_t)n);
        if (e) return e;
        at += (size_t)n;
    }
    return 0;
}
static int64_t do_sendmsg(GnuProcess* p, SnxSocket* s, uint64_t msg, uint64_t flags) {
    MsgHdr h;
    unsigned char vec[1024 * 16];
    int64_t r = read_msghdr(&p->memory, msg, &h), total;
    if (r) return r;
    if ((total = iov_total(&p->memory, &h, vec)) < 0) return total;
    SnxSockAddr to;
    if (h.name) {
        int e = addr_in(&p->memory, h.name, h.namelen, &to);
        if (e) return e == 1 ? -GNU_ENOENT : e;
    }
    unsigned char* buf = malloc(total ? (size_t)total : 1);
    if (!buf) return -GNU_ENOMEM;
    r = copy_vectors(&p->memory, &h, vec, buf, (size_t)total, 1);
    if (!r) r = snx_socket_send(s, buf, (size_t)total, gnu_linux_msg_in(flags), h.name ? &to : NULL);
    free(buf);
    return r;
}
static int64_t do_recvmsg(GnuProcess* p, SnxSocket* s, uint64_t msg, uint64_t flags) {
    MsgHdr h;
    unsigned char vec[1024 * 16];
    int64_t r = read_msghdr(&p->memory, msg, &h), total;
    if (r) return r;
    if ((total = iov_total(&p->memory, &h, vec)) < 0) return total;
    unsigned char* buf = malloc(total ? (size_t)total : 1);
    if (!buf) return -GNU_ENOMEM;
    SnxSockAddr from;
    int out_flags = 0;
    r = snx_socket_recv(s, buf, (size_t)total, gnu_linux_msg_in(flags), h.name ? &from : NULL, &out_flags);
    if (r >= 0) {
        int e = copy_vectors(&p->memory, &h, vec, buf, (size_t)r, 0);
        unsigned char b[8] = {0};
        if (!e && h.name) {
            /* namelen is in the guest's msghdr, at offset 8 */
            e = addr_out(&p->memory, &from, h.name, msg + 8);
        }
        if (!e) e = gnu_memory_write(&p->memory, msg + 40, b, 8); /* msg_controllen: no control messages */
        gnu_vfs_put32(b, gnu_linux_msg_out(out_flags));
        if (!e) e = gnu_memory_write(&p->memory, msg + 48, b, 4);
        if (e) r = e;
    }
    free(buf);
    return r;
}
static int64_t sys_mmsg(GnuProcess* p, const uint64_t a[6], int send) {
    SOCK_OR_FAIL(a[0]);
    uint64_t vlen = a[2] > 1024 ? 1024 : a[2], flags = a[3];
    int64_t done = 0;
    for (uint64_t i = 0; i < vlen; i++) {
        uint64_t m = a[1] + i * MMSGHDR_SIZE;
        /* recvmmsg with MSG_WAITFORONE waits for the first message only */
        uint64_t f = !send && done && (flags & LINUX_MSG_WAITFORONE) ? flags | LINUX_MSG_DONTWAIT : flags;
        int64_t r = send ? do_sendmsg(p, s, m, f) : do_recvmsg(p, s, m, f);
        if (r < 0) return done ? done : r;
        unsigned char len[4];
        gnu_vfs_put32(len, (uint32_t)r);
        if (gnu_memory_write(&p->memory, m + MSGHDR_SIZE, len, 4)) return done ? done : -GNU_EFAULT;
        done++;
    }
    return done;
}
static int64_t sys_eventfd2(GnuProcess* p, const uint64_t a[6]) {
    if (a[1] & ~(uint64_t)(LINUX_EFD_SEMAPHORE | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) return -GNU_EINVAL;
    SnxSocket* s;
    int r = snx_socket_eventfd(&s, (uint32_t)a[0], (a[1] & LINUX_EFD_SEMAPHORE) != 0);
    if (r) return r;
    return gnu_vfs_install_object(&p->vfs, &g_eventfd_ops, s, (a[1] & LINUX_SOCK_NONBLOCK) ? GNU_O_NONBLOCK : 0,
                                  (a[1] & LINUX_SOCK_CLOEXEC) != 0);
}

bool gnu_net_syscall(GnuProcess* p, uint64_t number, const uint64_t a[6], int64_t* result) {
    switch (number) {
        case GNU_NR_socket: *result = sys_socket(p, a); return true;
        case GNU_NR_socketpair: *result = sys_socketpair(p, a); return true;
        case GNU_NR_bind: *result = sys_bind_connect(p, a, 0); return true;
        case GNU_NR_connect: *result = sys_bind_connect(p, a, 1); return true;
        case GNU_NR_listen: {
            int64_t e;
            SnxSocket* s = sock_of(p, a[0], &e);
            *result = s ? snx_socket_listen(s, (int)a[1]) : e;
            return true;
        }
        case GNU_NR_accept: *result = sys_accept4(p, a, 0); return true;
        case GNU_NR_accept4: *result = sys_accept4(p, a, a[3]); return true;
        case GNU_NR_getsockname: *result = sys_name(p, a, 0); return true;
        case GNU_NR_getpeername: *result = sys_name(p, a, 1); return true;
        case GNU_NR_sendto: *result = sys_sendto(p, a); return true;
        case GNU_NR_recvfrom: *result = sys_recvfrom(p, a); return true;
        case GNU_NR_setsockopt: *result = sys_setsockopt(p, a); return true;
        case GNU_NR_getsockopt: *result = sys_getsockopt(p, a); return true;
        case GNU_NR_shutdown: {
            int64_t e;
            SnxSocket* s = sock_of(p, a[0], &e);
            *result = s ? snx_socket_shutdown(s, (int)a[1]) : e;
            return true;
        }
        case GNU_NR_sendmsg:
        case GNU_NR_recvmsg: {
            int64_t e;
            SnxSocket* s = sock_of(p, a[0], &e);
            *result = !s ? e : number == GNU_NR_sendmsg ? do_sendmsg(p, s, a[1], a[2]) : do_recvmsg(p, s, a[1], a[2]);
            return true;
        }
        case GNU_NR_sendmmsg: *result = sys_mmsg(p, a, 1); return true;
        case GNU_NR_recvmmsg: *result = sys_mmsg(p, a, 0); return true;
        case GNU_NR_eventfd2: *result = sys_eventfd2(p, a); return true;
        case GNU_NR_ppoll: *result = gnu_net_ppoll(p, a); return true;
        case GNU_NR_pselect6: *result = gnu_net_pselect6(p, a); return true;
        case GNU_NR_epoll_create1: *result = gnu_net_epoll_create1(p, a[0]); return true;
        case GNU_NR_epoll_ctl: *result = gnu_net_epoll_ctl(p, a); return true;
        case GNU_NR_epoll_pwait: *result = gnu_net_epoll_pwait(p, a, 0); return true;
        case GNU_NR_epoll_pwait2: *result = gnu_net_epoll_pwait(p, a, 1); return true;
    }
    return false;
}
