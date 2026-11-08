/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Optional static AArch64 libc fixture; see docs/platforms/nx/networking.md. Loopback only, no external DNS or ping required. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL %d: %s (errno %d %s)\n", __LINE__, #x, errno, strerror(errno));                      \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
int main(void) {
    /* TCP over loopback, one thread */
    int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(l >= 0);
    int one = 1;
    CHECK(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(l, (struct sockaddr*)&a, sizeof a) == 0);
    CHECK(listen(l, 4) == 0);
    socklen_t al = sizeof a;
    CHECK(getsockname(l, (struct sockaddr*)&a, &al) == 0 && al == sizeof a && a.sin_port != 0);
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(c >= 0);
    int r = connect(c, (struct sockaddr*)&a, sizeof a);
    CHECK(r == 0 || errno == EINPROGRESS);
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int s = accept4(l, (struct sockaddr*)&peer, &pl, 0);
    CHECK(s >= 0 && pl == sizeof peer && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    struct pollfd pf = {c, POLLOUT, 0};
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLOUT));
    int err = -1;
    socklen_t el = sizeof err;
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    char buf[256];
    CHECK(recv(c, buf, sizeof buf, 0) == -1 && errno == EAGAIN);
    CHECK(write(s, "hello", 5) == 5);
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN));
    int avail = 0;
    CHECK(ioctl(c, FIONREAD, &avail) == 0 && avail == 5);
    CHECK(read(c, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    struct iovec iov[2] = {{"ab", 2}, {"cd", 2}};
    CHECK(writev(c, iov, 2) == 4);
    CHECK(recv(s, buf, 4, MSG_WAITALL) == 4 && !memcmp(buf, "abcd", 4));
    /* receive timeout on a blocking socket */
    struct timeval tv = {0, 200000};
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    struct timeval got;
    socklen_t gl = sizeof got;
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &got, &gl) == 0 && got.tv_usec == 200000);
    CHECK(recv(s, buf, 1, 0) == -1 && errno == EAGAIN);
    int type = 0;
    socklen_t tl = sizeof type;
    CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM);
    CHECK(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    struct stat st;
    CHECK(fstat(s, &st) == 0 && S_ISSOCK(st.st_mode));
    char link[64] = {0};
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", s);
    CHECK(readlink(path, link, sizeof link) > 0 && !strncmp(link, "socket:[", 8));
    /* sendmsg/recvmsg */
    struct msghdr m = {0};
    struct iovec v = {"msg!", 4};
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    CHECK(sendmsg(c, &m, MSG_NOSIGNAL) == 4);
    char rb[8] = {0};
    struct iovec rv = {rb, 8};
    struct msghdr rm = {0};
    rm.msg_iov = &rv;
    rm.msg_iovlen = 1;
    CHECK(recvmsg(s, &rm, 0) == 4 && !memcmp(rb, "msg!", 4));
    /* epoll */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0x1234567890ull};
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == -1 && errno == EEXIST);
    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 50) == 0);
    CHECK(send(c, "x", 1, 0) == 1);
    CHECK(epoll_wait(ep, out, 4, 2000) == 1 && out[0].data.u64 == 0x1234567890ull && (out[0].events & EPOLLIN));
    CHECK(recv(s, buf, 1, 0) == 1);
    /* select */
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    struct timeval z = {0, 0};
    CHECK(select(s + 1, &rs, NULL, NULL, &z) == 0);
    CHECK(send(c, "y", 1, 0) == 1);
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    z.tv_sec = 2;
    CHECK(select(s + 1, &rs, NULL, NULL, &z) == 1 && FD_ISSET(s, &rs));
    CHECK(recv(s, buf, 1, 0) == 1);
    /* EOF after shutdown */
    CHECK(shutdown(c, SHUT_WR) == 0);
    CHECK(recv(s, buf, 1, 0) == 0);
    close(c);
    close(s);
    close(l);
    close(ep);
    /* UDP */
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u1 >= 0 && u2 >= 0);
    struct sockaddr_in ua = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(u1, (struct sockaddr*)&ua, sizeof ua) == 0);
    al = sizeof ua;
    CHECK(getsockname(u1, (struct sockaddr*)&ua, &al) == 0);
    CHECK(sendto(u2, "dgram", 5, 0, (struct sockaddr*)&ua, sizeof ua) == 5);
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    CHECK(recvfrom(u1, buf, 3, 0, (struct sockaddr*)&from, &fl) == 3 && fl == sizeof from &&
          from.sin_family == AF_INET);
    close(u1);
    close(u2);
    /* socketpair and eventfd */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[0], "pair", 4) == 4 && read(sv[1], buf, 4) == 4 && !memcmp(buf, "pair", 4));
    close(sv[0]);
    close(sv[1]);
    int e = eventfd(0, EFD_NONBLOCK);
    CHECK(e >= 0);
    uint64_t val = 0;
    CHECK(read(e, &val, 8) == -1 && errno == EAGAIN);
    val = 3;
    CHECK(write(e, &val, 8) == 8);
    struct pollfd ep2 = {e, POLLIN, 0};
    CHECK(poll(&ep2, 1, 0) == 1);
    CHECK(read(e, &val, 8) == 8 && val == 3);
    close(e);
    /* non-socket fd */
    CHECK(send(1, "x", 1, 0) == -1 && errno == ENOTSOCK);
    /* AF_UNIX paths stay out */
    int ux = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(ux >= 0);
    close(ux);
    /* names: /etc/hosts */
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    CHECK(getaddrinfo("localhost", "http", &hints, &res) == 0 && res &&
          ((struct sockaddr_in*)res->ai_addr)->sin_port == htons(80));
    if (res) freeaddrinfo(res);
    puts("network test: ok");
    return 0;
}
