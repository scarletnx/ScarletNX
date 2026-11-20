/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L /* sockets, select and clock_gettime under -std=c23 */
#endif
#include "common/online/socket.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static int started(char* why, size_t size) {
    static int done;
    WSADATA data;
    if (done) return 0;
    int e = WSAStartup(MAKEWORD(2, 2), &data);
    if (e) {
        snprintf(why, size, "Winsock 2.2 does not start: error %d", e);
        return -1;
    }
    done = 1;
    return 0;
}
static int error_code(void) { return WSAGetLastError(); }
#define FD(s)   ((SOCKET)(s)->handle)
#define REFUSED WSAECONNRESET /* a datagram's port closed: Winsock's word for it */
#else
static int started(char* why, size_t size) {
    (void)why, (void)size;
    return 0;
}
static int error_code(void) { return errno; }
#define FD(s)   ((s)->fd)
#define REFUSED ECONNREFUSED
#endif

static void say(char* why, size_t size, const char* what, const uint8_t ip[4], uint16_t port) {
    snprintf(why, size, "%s %u.%u.%u.%u:%u: error %d", what, ip[0], ip[1], ip[2], ip[3], port, error_code());
}

int snx_online_udp_connect(SnxOnlineSocket* s, const uint8_t ip[4], uint16_t port, char* why, size_t size) {
    struct sockaddr_in to = {0};
    if (started(why, size)) return -1;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    memcpy(&to.sin_addr, ip, 4);
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        say(why, size, "no UDP socket for", ip, port);
        return -1;
    }
    s->handle = (uintptr_t)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        say(why, size, "no UDP socket for", ip, port);
        return -1;
    }
    s->fd = fd;
#endif
    if (connect(FD(s), (const struct sockaddr*)&to, sizeof to)) {
        say(why, size, "a UDP socket does not take", ip, port);
        snx_online_socket_close(s);
        return -1;
    }
    return 0;
}

int snx_online_udp_send(SnxOnlineSocket* s, const void* data, size_t length, char* why, size_t size) {
    if (send(FD(s), (const char*)data, (int)length, 0) != (int)length) {
        snprintf(why, size, "sending %u bytes failed: error %d", (unsigned)length, error_code());
        return -1;
    }
    return 0;
}

int snx_online_udp_receive(SnxOnlineSocket* s, void* data, size_t cap, int timeout_ms, char* why, size_t size) {
    fd_set set;
    struct timeval wait;
    FD_ZERO(&set);
    FD_SET(FD(s), &set);
    wait.tv_sec = timeout_ms / 1000;
    wait.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select((int)FD(s) + 1, &set, NULL, NULL, &wait);
    if (ready < 0) {
        snprintf(why, size, "waiting for a reply failed: error %d", error_code());
        return -1;
    }
    if (!ready) return 0;
    int got = (int)recv(FD(s), (char*)data, (int)cap, 0);
    if (got < 0) {
        snprintf(why, size, "reading the reply failed: error %d (the server's port is closed when it is %d)",
                 error_code(), REFUSED);
        return -1;
    }
    return got;
}

int snx_online_udp_wait_any(SnxOnlineSocket* s, size_t count, int timeout_ms, char* why, size_t size) {
    fd_set set;
    struct timeval wait;
    int most = 0;
    FD_ZERO(&set);
    for (size_t i = 0; i < count; ++i) {
        FD_SET(FD(&s[i]), &set);
        if ((int)FD(&s[i]) > most) most = (int)FD(&s[i]);
    }
    wait.tv_sec = timeout_ms / 1000;
    wait.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select(most + 1, &set, NULL, NULL, &wait);
    if (ready < 0) {
        snprintf(why, size, "waiting for replies failed: error %d", error_code());
        return -2;
    }
    for (size_t i = 0; ready && i < count; ++i)
        if (FD_ISSET(FD(&s[i]), &set)) return (int)i;
    return -1;
}

void snx_online_socket_close(SnxOnlineSocket* s) {
#ifdef _WIN32
    closesocket(FD(s));
    s->handle = (uintptr_t)INVALID_SOCKET;
#else
    close(s->fd);
    s->fd = -1;
#endif
}

uint64_t snx_online_clock_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}
