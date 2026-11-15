/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LINUX_POLL(guest, NAME): the guest's poll and epoll events as the stack's SNX_POLL##NAME (epoll's EPOLLIN... are
   poll's values) */
LINUX_POLL(0x0001, IN)
LINUX_POLL(0x0002, PRI)
LINUX_POLL(0x0004, OUT)
LINUX_POLL(0x0008, ERR)
LINUX_POLL(0x0010, HUP)
LINUX_POLL(0x0020, NVAL)
LINUX_POLL(0x0040, RDNORM)
LINUX_POLL(0x0080, RDBAND)
LINUX_POLL(0x0100, WRNORM)
LINUX_POLL(0x0200, WRBAND)
LINUX_POLL(0x2000, RDHUP)
#undef LINUX_POLL
