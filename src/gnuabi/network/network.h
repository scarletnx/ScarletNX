/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's networking on the emulated network stack (src/common/network): sockets and eventfd (socket.c), poll,
   select and epoll (poll.c), and the resolver's files under /etc (etc.c). Descriptors are the VFS's, backed by the
   stack's objects (vfs.h's GnuObjectOps). The guest's errno numbering is the stack's, Linux's. A call blocks the
   guest thread that makes it, as a host thread would block, and a signal stops it with EINTR (signals.c). */
#ifndef SNX_GNU_NETWORK_H
#define SNX_GNU_NETWORK_H
#include "gnuabi/process/process.h"
#include "common/network/net.h"
#include <stdbool.h>

enum {
#define LINUX_NET_SYSCALL(number, name) GNU_NR_##name = number,
#include "gnuabi/abi/linux_syscalls.inc"
};

/* syscalls.c: true when number is one of abi/linux_syscalls.inc's */
bool gnu_net_syscall(GnuProcess* process, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* etc.c: /etc/resolv.conf from the host's DNS servers, /etc/hosts, /etc/services... */
int gnu_net_process_init(GnuProcess* process);

/* socket.c, for poll.c: the stack's socket behind a socket or eventfd descriptor, NULL for the others */
SnxSocket* gnu_net_pollable(const GnuFdInfo* info);
uint16_t gnu_net_poll_in(uint32_t guest);
uint32_t gnu_net_poll_out(uint16_t events);

/* poll.c */
int64_t gnu_net_ppoll(GnuProcess* process, const uint64_t arguments[6]);
int64_t gnu_net_pselect6(GnuProcess* process, const uint64_t arguments[6]);
int64_t gnu_net_epoll_create1(GnuProcess* process, uint64_t flags);
int64_t gnu_net_epoll_ctl(GnuProcess* process, const uint64_t arguments[6]);
int64_t gnu_net_epoll_pwait(GnuProcess* process, const uint64_t arguments[6], int timespec_timeout);
#endif
