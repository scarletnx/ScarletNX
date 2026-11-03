/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The stack's constants (net.h, Linux's values for most) as the host's and back, from the tables in abi/: for the
   stack's own files, and for an HLE layer that calls the host's socket API itself (the Android bridge, whose
   descriptors are the host's) */
#ifndef SNX_NET_HOST_H
#define SNX_NET_HOST_H
#include "common/network/net.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef int snx_socklen;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
typedef socklen_t snx_socklen;
#endif

/* a host errno (POSIX) or Winsock error, -SNX_E* */
int snx_net_errno_from_host(int e);
/* the address conversions, 0 or -SNX_EAFNOSUPPORT */
int snx_net_addr_to_host(const SnxSockAddr* in, struct sockaddr_storage* out, snx_socklen* size);
int snx_net_addr_from_host(const struct sockaddr* in, snx_socklen size, SnxSockAddr* out);
int snx_net_domain_to_host(int domain); /* -1 for none */
int snx_net_domain_from_host(int domain);
int snx_net_socktype_to_host(int type); /* -1 for none */
int snx_net_socktype_from_host(int type);
int snx_net_level_to_host(int level); /* -1 for none */
/* SNX_MSG_* as the host's send and receive flags, and back */
int snx_net_msg_to_host(int flags);
int snx_net_msg_from_host(int host);
/* SNX_POLL* as the host's poll events (WSAPoll's on Windows), and back */
short snx_net_poll_to_host(unsigned events);
uint16_t snx_net_poll_from_host(short host);
/* an option (SNX_SOL_*, SNX_SO_*...) as the host's level and name: 0, or -SNX_ENOPROTOOPT for one the host lacks.
   Its value's layout is abi/sockopt.inc's kind */
int snx_net_sockopt_to_host(int level, int name, int* host_level, int* host_name);
#endif
