/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GNU ABI's socket, poll and epoll syscalls, called the way a guest's libc calls them, over the host's loopback.
   No guest code runs: the guest's memory and descriptor table are set up and the syscalls dispatched directly. */
#include "gnuabi/process/process.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/network/network.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "gnuabi_network.c:%d: check failed: %s\n", __LINE__, #x);                                  \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static GnuProcess process;
static GnuThread* thread;
enum { BUF = 0x10000, ADDR = 0x10000, ADDRLEN = 0x10100, DATA = 0x11000, POLLFDS = 0x12000, PATH = 0x13000 };
static int64_t sys(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    uint64_t args[6] = {a, b, c, d, e, f};
    return gnu_syscall(thread, n, args);
}
static void put(uint64_t at, const void* v, size_t n) { CHECK(!gnu_memory_write(&process.memory, at, v, n)); }
static void get(uint64_t at, void* v, size_t n) { CHECK(!gnu_memory_read(&process.memory, at, v, n)); }
/* the guest's ints, little-endian on any host */
static void put32(uint64_t at, uint32_t v) {
    v = snx_le32(v);
    put(at, &v, 4);
}
static uint32_t get32(uint64_t at) {
    uint32_t v = 0;
    get(at, &v, 4);
    return snx_le32(v);
}
static void quiet(const char* m) { (void)m; }
/* a Linux sockaddr_in for 127.0.0.1:port (port in network order) at ADDR */
static void loopback(uint16_t port) {
    unsigned char a[16] = {2, 0, (unsigned char)port, (unsigned char)(port >> 8), 127, 0, 0, 1};
    put(ADDR, a, sizeof a);
}

int main(void) {
    snx_net_set_log(quiet);
    gnu_kernel_init(&process.kernel, &gnu_kernel_ops, &process, NULL, 1);
    thread = gnu_thread_new(&process, 1);
    CHECK(thread);
    CHECK(!gnu_memory_init(&process.memory));
    process.kernel.base = process.memory.base;
    CHECK(!gnu_memory_map(&process.memory, BUF, 0x10000, GNU_PROT_READ | GNU_PROT_WRITE, 0));
    CHECK(!gnu_vfs_init(&process.vfs));
    CHECK(!gnu_net_process_init(&process));

    /* a listener, and the address it got */
    int l = (int)sys(GNU_NR_socket, 2, 1 | 0x80000, 0, 0, 0, 0); /* AF_INET, SOCK_STREAM | SOCK_CLOEXEC */
    CHECK(l >= 3);
    loopback(0);
    CHECK(sys(GNU_NR_bind, (uint64_t)l, ADDR, 16, 0, 0, 0) == 0);
    CHECK(sys(GNU_NR_listen, (uint64_t)l, 8, 0, 0, 0, 0) == 0);
    put32(ADDRLEN, 16);
    CHECK(sys(GNU_NR_getsockname, (uint64_t)l, ADDR, ADDRLEN, 0, 0, 0) == 0 && get32(ADDRLEN) == 16);
    unsigned char sa[16];
    get(ADDR, sa, 16);
    uint16_t port = (uint16_t)(sa[2] | sa[3] << 8);
    CHECK(sa[0] == 2 && sa[1] == 0 && port && sa[4] == 127);
    CHECK(sys(25, (uint64_t)l, 1, 0, 0, 0, 0) == 1); /* F_GETFD: FD_CLOEXEC */

    /* a non-blocking client, accepted, and data both ways through read() and write() */
    int c = (int)sys(GNU_NR_socket, 2, 1 | 0x800, 0, 0, 0, 0);
    CHECK(c > l);
    CHECK((sys(25, (uint64_t)c, 3, 0, 0, 0, 0) & 0x800) == 0x800); /* F_GETFL: O_NONBLOCK */
    loopback(port);
    int64_t r = sys(GNU_NR_connect, (uint64_t)c, ADDR, 16, 0, 0, 0);
    CHECK(r == 0 || r == -115);
    put32(ADDRLEN, 16);
    int s = (int)sys(GNU_NR_accept4, (uint64_t)l, ADDR, ADDRLEN, 0, 0, 0);
    CHECK(s > c && get32(ADDRLEN) == 16);
    unsigned char pfd[8] = {0};
    snx_st_le32(pfd, (uint32_t)c);
    pfd[4] = 4; /* POLLOUT */
    put(POLLFDS, pfd, 8);
    CHECK(sys(GNU_NR_ppoll, POLLFDS, 1, 0, 0, 0, 0) == 1);
    get(POLLFDS, pfd, 8);
    CHECK(pfd[6] & 4);
    put(DATA, "ping", 4);
    CHECK(sys(64, (uint64_t)c, DATA, 4, 0, 0, 0) == 4);       /* write */
    CHECK(sys(63, (uint64_t)s, DATA + 16, 16, 0, 0, 0) == 4); /* read */
    char got[8] = {0};
    get(DATA + 16, got, 4);
    CHECK(!memcmp(got, "ping", 4));
    CHECK(sys(63, (uint64_t)c, DATA, 4, 0, 0, 0) == -GNU_EAGAIN); /* nothing for the non-blocking client */
    CHECK(sys(62, (uint64_t)s, 0, 0, 0, 0, 0) == -GNU_ESPIPE);    /* lseek */

    /* fstat, /proc/self/fd, dup */
    unsigned char st[128];
    CHECK(sys(80, (uint64_t)s, DATA, 0, 0, 0, 0) == 0);
    get(DATA, st, sizeof st);
    CHECK(((st[16] | st[17] << 8 | st[18] << 16) & 0170000) == 0140000);
    char path[32];
    snprintf(path, sizeof path, "/proc/self/fd/%d", s);
    put(PATH, path, strlen(path) + 1);
    r = sys(78, (uint64_t)-100, PATH, DATA, 64, 0, 0); /* readlinkat */
    get(DATA, got, 8);
    CHECK(r > 8 && !memcmp(got, "socket:[", 8));
    int d = (int)sys(23, (uint64_t)s, 0, 0, 0, 0, 0); /* dup */
    CHECK(d > s && sys(57, (uint64_t)s, 0, 0, 0, 0, 0) == 0);
    CHECK(sys(GNU_NR_sendto, (uint64_t)d, DATA, 1, 0, 0, 0) == 1); /* the socket outlives one of its descriptors */

    /* options */
    unsigned char tv[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0xA1, 0x07}; /* 0 s, 500000 us */
    put(DATA, tv, 16);
    CHECK(sys(GNU_NR_setsockopt, (uint64_t)d, 1, 20, DATA, 16, 0) == 0); /* SO_RCVTIMEO */
    put32(ADDRLEN, 16);
    CHECK(sys(GNU_NR_getsockopt, (uint64_t)d, 1, 20, DATA + 32, ADDRLEN, 0) == 0 && get32(DATA + 40) == 500000);
    put32(ADDRLEN, 4);
    CHECK(sys(GNU_NR_getsockopt, (uint64_t)d, 1, 3, DATA, ADDRLEN, 0) == 0 && get32(DATA) == 1); /* SO_TYPE */
    CHECK(sys(GNU_NR_setsockopt, (uint64_t)d, 1, 4242, DATA, 4, 0) == -SNX_ENOPROTOOPT);

    /* epoll and pselect6 */
    int ep = (int)sys(GNU_NR_epoll_create1, 0, 0, 0, 0, 0, 0);
    CHECK(ep > 0);
    unsigned char ev[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0xEF, 0xBE, 0xAD, 0xDE};
    put(DATA, ev, 16);
    CHECK(sys(GNU_NR_epoll_ctl, (uint64_t)ep, 1, (uint64_t)c, DATA, 0, 0) == 0);
    /* the byte sent through d: FreeBSD's loopback hands it over microseconds after sendto, so the wait has a timeout */
    CHECK(sys(GNU_NR_epoll_pwait, (uint64_t)ep, DATA + 64, 4, 1000, 0, 0) == 1);
    CHECK(get32(DATA + 64) & 1 && get32(DATA + 72) == 0xDEADBEEF);
    CHECK(sys(GNU_NR_epoll_ctl, (uint64_t)ep, 1, (uint64_t)c, DATA, 0, 0) == -GNU_EEXIST);
    CHECK(sys(GNU_NR_recvfrom, (uint64_t)c, DATA, 16, 0, 0, 0) == 1);
    CHECK(sys(GNU_NR_epoll_pwait, (uint64_t)ep, DATA + 64, 4, 0, 0, 0) == 0);
    unsigned char set[8] = {0};
    set[c / 8] = (unsigned char)(1 << (c % 8));
    put(DATA, set, 8);
    unsigned char zero[16] = {0};
    put(DATA + 32, zero, 16);
    CHECK(sys(GNU_NR_pselect6, (uint64_t)c + 1, DATA, 0, 0, DATA + 32, 0) == 0);

    /* eventfd */
    int e = (int)sys(GNU_NR_eventfd2, 5, 0x800, 0, 0, 0, 0);
    CHECK(e > 0);
    CHECK(sys(63, (uint64_t)e, DATA, 8, 0, 0, 0) == 8 && get32(DATA) == 5);
    CHECK(sys(63, (uint64_t)e, DATA, 8, 0, 0, 0) == -GNU_EAGAIN);

    /* what isn't a socket, and what the guest can't reach */
    CHECK(sys(GNU_NR_recvfrom, 1, DATA, 1, 0, 0, 0) == -SNX_ENOTSOCK);
    CHECK(sys(GNU_NR_recvfrom, 200, DATA, 1, 0, 0, 0) == -GNU_EBADF);
    CHECK(sys(GNU_NR_socket, 16, 3, 0, 0, 0, 0) == -SNX_EAFNOSUPPORT); /* AF_NETLINK */
    int u = (int)sys(GNU_NR_socket, 1, 1, 0, 0, 0, 0);
    unsigned char un[16] = {1, 0, '/', 't', 'm', 'p', '/', 'x'};
    put(ADDR, un, sizeof un);
    CHECK(u > 0 && sys(GNU_NR_connect, (uint64_t)u, ADDR, sizeof un, 0, 0, 0) == -GNU_ENOENT);
    CHECK(sys(GNU_NR_socketpair, 1, 1, 0, DATA, 0, 0) == 0 && get32(DATA) > 0 && get32(DATA + 4) > get32(DATA));

    /* the resolver's files */
    put(PATH, "/etc/hosts", 11);
    int h = (int)sys(56, (uint64_t)-100, PATH, 0, 0, 0, 0);
    CHECK(h > 0 && sys(63, (uint64_t)h, DATA, 64, 0, 0, 0) > 10);
    get(DATA, got, 8);
    CHECK(!memcmp(got, "127.0.0", 7));
    put(PATH, "/etc/resolv.conf", 17);
    CHECK(sys(56, (uint64_t)-100, PATH, 0, 0, 0, 0) > 0);
    CHECK(sys(56, (uint64_t)-100, PATH, 2, 0, 0, 0) == -GNU_EACCES); /* read only */

    gnu_vfs_destroy(&process.vfs);
    gnu_memory_destroy(&process.memory);
    if (failures) {
        fprintf(stderr, "gnuabi_network: %u checks failed\n", failures);
        return 1;
    }
    puts("gnuabi_network: ok");
    return 0;
}
