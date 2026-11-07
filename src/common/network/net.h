/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The emulated network stack the HLE layers share (nx's bsd and sfdnsres, the GNU ABI's socket syscalls). Every
   socket is a host socket, except ICMP: a guest's raw ICMP socket needs privileges the runtime doesn't have, so its
   echo requests are answered through an unprivileged ping socket or the host's ping (net_icmp.c).

   The interface uses the stack's own constants (the tables in abi/), Linux's values for most, and each HLE layer translates
   its guest's ABI to them with tables of its own. Calls return 0 or a count on success and -SNX_E* on failure.
   Sockets are reference counted: a guest's descriptor table holds one reference, and a call in progress another,
   so closing a descriptor while another guest thread blocks on it is safe.

   Blocking is emulated: host sockets are always non-blocking, and a call that would block waits in poll() for the
   socket and for snx_net_cancel(), within the socket's SO_RCVTIMEO or SO_SNDTIMEO. Calls never hold a lock while
   they wait, so the HLE layers call them from the guest thread that asked. */
#ifndef SNX_NET_H
#define SNX_NET_H
#include <stddef.h>
#include <stdint.h>

enum {
#define SNX_NET_ERRNO(name, value) SNX_E##name = value,
#include "common/network/abi/errno.inc"
#define SNX_NET_DOMAIN(name, value, host) SNX_AF_##name = value,
#include "common/network/abi/domain.inc"
#define SNX_NET_SOCKTYPE(name, value, host) SNX_SOCK_##name = value,
#include "common/network/abi/socktype.inc"
#define SNX_NET_LEVEL(name, value, host) SNX_SOL_##name = value,
#include "common/network/abi/level.inc"
#define SNX_NET_SOCKOPT(level, name, value, kind) SNX_##name = value,
#include "common/network/abi/sockopt.inc"
#define SNX_NET_MSG(name, value, host) SNX_MSG_##name = value,
#include "common/network/abi/msgflag.inc"
#define SNX_NET_POLL(name, value, host, wsapoll) SNX_POLL##name = value,
#include "common/network/abi/poll.inc"
#define SNX_NET_GAI(name, value, host, text) SNX_EAI_##name = value,
#include "common/network/abi/gai.inc"
#define SNX_NET_AIFLAG(name, value, host) SNX_AI_##name = value,
#include "common/network/abi/aiflag.inc"
#define SNX_NET_NIFLAG(name, value, host) SNX_NI_##name = value,
#include "common/network/abi/niflag.inc"
    SNX_SHUT_RD = 0,
    SNX_SHUT_WR = 1,
    SNX_SHUT_RDWR = 2,
    /* IANA protocol numbers, the same on every host */
    SNX_IPPROTO_IP = 0,
    SNX_IPPROTO_ICMP = 1,
    SNX_IPPROTO_TCP = 6,
    SNX_IPPROTO_UDP = 17,
    SNX_IPPROTO_ICMPV6 = 58,
    SNX_IPPROTO_RAW = 255,
};

/* An address in the stack's terms. port and the address bytes are in network order, as guests keep them */
typedef struct SnxSockAddr {
    uint16_t family; /* SNX_AF_* */
    uint16_t port;
    uint32_t flowinfo; /* IPv6 */
    uint8_t addr[16];  /* IPv4 in the first 4 bytes */
    uint32_t scope_id; /* IPv6 */
} SnxSockAddr;

/* option values (abi/sockopt.inc's kinds) */
typedef struct SnxLinger {
    int32_t onoff, linger;
} SnxLinger;
typedef struct SnxTimeval {
    int64_t sec, usec;
} SnxTimeval;
typedef struct SnxIpMreq {
    uint8_t multiaddr[4], interface[4];
} SnxIpMreq;
typedef struct SnxIpv6Mreq {
    uint8_t multiaddr[16];
    uint32_t ifindex;
} SnxIpv6Mreq;

typedef struct SnxSocket SnxSocket;

/* Starts the stack (Winsock, the cancel pipe). Every user calls it, the first call does the work. 0 or -SNX_E* */
int snx_net_init(void);
/* Wakes every call that waits, and makes later waits fail with SNX_EINTR until snx_net_restart: for a guest that
   exits, or a debugger that stops it */
void snx_net_cancel(void);
void snx_net_restart(void);
/* A wake of one thread: while it is signalled, the waits of the thread it is bound to (snx_net_poll and the blocking
   socket calls) end at once with -SNX_EINTR, as snx_net_cancel ends every thread's. A guest's signal to a thread that
   waits stops its wait this way, with no race against the wait's start: the wake is a pipe that stays readable until
   the thread drains it. snx_net_wake_signal is one write, safe in a host signal handler */
typedef struct SnxNetWake SnxNetWake;
SnxNetWake* snx_net_wake_new(void); /* NULL when the host has no pipe for it */
void snx_net_wake_free(SnxNetWake* wake);
void snx_net_wake_signal(SnxNetWake* wake);
void snx_net_wake_drain(SnxNetWake* wake);
/* the calling thread's wake, NULL for none */
void snx_net_wake_bind(SnxNetWake* wake);
/* Where the stack's messages go: stderr by default. The runtime points them at its log, and the warnings, what the
   player must hear of (a room that put the console out), at its warnings */
void snx_net_set_log(void (*log)(const char* message));
void snx_net_set_warn(void (*warn)(const char* message));

/* Sockets. type is SNX_SOCK_*, without flags. A raw or datagram ICMP socket the host refuses becomes an emulated one */
int snx_socket_open(SnxSocket** out, int domain, int type, int protocol);
int snx_socket_pair(SnxSocket* out[2], int domain, int type, int protocol);
/* An eventfd: a 64-bit counter read and written as 8 bytes, readable while it isn't 0 */
int snx_socket_eventfd(SnxSocket** out, uint64_t initial, int semaphore);
SnxSocket* snx_socket_ref(SnxSocket* s);
void snx_socket_unref(SnxSocket* s); /* the last reference closes the socket */
int snx_socket_domain(const SnxSocket* s);
int snx_socket_type(const SnxSocket* s);
int snx_socket_protocol(const SnxSocket* s);
int snx_socket_emulated(const SnxSocket* s); /* 1 for the ICMP and eventfd emulations */
int snx_socket_nonblocking(const SnxSocket* s);
int snx_socket_set_nonblocking(SnxSocket* s, int on);

int snx_socket_bind(SnxSocket* s, const SnxSockAddr* addr);
int snx_socket_connect(SnxSocket* s, const SnxSockAddr* addr);
int snx_socket_listen(SnxSocket* s, int backlog);
/* peer may be NULL. The new socket is non-blocking if nonblocking is set (BSD inherits, Linux asks for it) */
int snx_socket_accept(SnxSocket* s, SnxSocket** out, SnxSockAddr* peer, int nonblocking);
int snx_socket_shutdown(SnxSocket* s, int how);
/* to and from may be NULL. flags are SNX_MSG_*. recv sets *out_flags (SNX_MSG_TRUNC) when out_flags isn't NULL */
int64_t snx_socket_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to);
int64_t snx_socket_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags);
int snx_socket_name(SnxSocket* s, SnxSockAddr* out);
int snx_socket_peer(SnxSocket* s, SnxSockAddr* out);
/* level and name are SNX_SOL_* and SNX_SO_*, SNX_IP_*...; the value is laid out as abi/sockopt.inc says, its
   numbers in the guests' byte order (little-endian) also when the runtime sets an option itself: the stack converts
   them on a big-endian host. getsockopt's *size is the buffer's size on entry and the value's on return */
int snx_socket_setsockopt(SnxSocket* s, int level, int name, const void* value, size_t size);
int snx_socket_getsockopt(SnxSocket* s, int level, int name, void* value, size_t* size);
int snx_socket_readable_bytes(SnxSocket* s, int* bytes); /* FIONREAD */

/* poll. An entry polls its socket, or the host descriptor host_fd when socket is NULL (POSIX only: the GNU ABI's
   stdio), or nothing when both are unset (host_fd -1): its revents stay 0. events are SNX_POLL*. timeout_ms < 0
   waits forever. Returns the number of entries with revents set */
typedef struct SnxPollEntry {
    SnxSocket* socket;
    intptr_t host_fd;
    uint16_t events, revents;
} SnxPollEntry;
int snx_net_poll(SnxPollEntry* entries, size_t count, int timeout_ms);

/* Names (net_dns.c). getaddrinfo returns 0 or SNX_EAI_*. */
typedef struct SnxAddrInfo {
    int flags, family, socktype, protocol;
    SnxSockAddr addr;
    char* canonname;
    struct SnxAddrInfo* next;
} SnxAddrInfo;
int snx_net_getaddrinfo(const char* host, const char* service, const SnxAddrInfo* hints, SnxAddrInfo** out);
void snx_net_freeaddrinfo(SnxAddrInfo* list);
int snx_net_getnameinfo(const SnxSockAddr* addr, char* host, size_t host_size, char* service, size_t service_size,
                        int flags);
const char* snx_net_gai_strerror(int error);
int snx_net_parse_ipv4(const char* text, uint8_t out[4]); /* 1 for a dotted IPv4 address */

/* The host's network (net_iface.c): its interfaces with an IPv4 address that are up, loopback left out, the one with
   the default route first */
typedef struct SnxNetInterface {
    char name[64];
    uint8_t ip[4], mask[4], gateway[4];
    int wireless;
} SnxNetInterface;
int snx_net_interfaces(SnxNetInterface* out, int max);
/* 0 and *out when the host has a connection, -SNX_ENETDOWN when it has none */
int snx_net_primary_interface(SnxNetInterface* out);
/* The host's DNS servers. IPv4 ones in ipv4 (up to max), the count returned. nameservers_text, when not NULL, gets
   resolv.conf lines for every server including IPv6 ones, and the host's search and options lines (the GNU ABI's
   /etc/resolv.conf) */
int snx_net_dns_servers(uint8_t (*ipv4)[4], int max, char* nameservers_text, size_t text_size);

#endif
