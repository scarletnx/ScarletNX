/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The few socket calls the library makes, the same on POSIX hosts and on Windows (Winsock) */
#ifndef SNX_ONLINE_SOCKET_H
#define SNX_ONLINE_SOCKET_H
#include <stddef.h>
#include <stdint.h>

typedef struct SnxOnlineSocket {
#ifdef _WIN32
    uintptr_t handle;
#else
    int fd;
#endif
} SnxOnlineSocket;

/* a UDP socket that sends to and hears only ip:port. 0, or -1 with why */
int snx_online_udp_connect(SnxOnlineSocket* s, const uint8_t ip[4], uint16_t port, char* why, size_t size);
/* sends length bytes: 0, or -1 with why */
int snx_online_udp_send(SnxOnlineSocket* s, const void* data, size_t length, char* why, size_t size);
/* waits up to timeout_ms for a datagram: its length, 0 at the time's end, -1 with why */
int snx_online_udp_receive(SnxOnlineSocket* s, void* data, size_t cap, int timeout_ms, char* why, size_t size);
/* waits up to timeout_ms for any of count sockets to have a datagram: its index, -1 at the time's end, -2 with why */
int snx_online_udp_wait_any(SnxOnlineSocket* s, size_t count, int timeout_ms, char* why, size_t size);
void snx_online_socket_close(SnxOnlineSocket* s);
/* milliseconds of a clock that only goes forward */
uint64_t snx_online_clock_ms(void);

#endif
