/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LINUX_MSG(guest, NAME): the guest's send and receive flags as the stack's SNX_MSG_##NAME.
   LINUX_MSG_IGNORED(guest, name): hints the stack has no use for (MSG_MORE is a corking hint, MSG_CONFIRM an ARP one) */
#ifndef LINUX_MSG
#define LINUX_MSG(guest, name)
#endif
#ifndef LINUX_MSG_IGNORED
#define LINUX_MSG_IGNORED(guest, name)
#endif
LINUX_MSG(0x00001, OOB)
LINUX_MSG(0x00002, PEEK)
LINUX_MSG(0x00004, DONTROUTE)
LINUX_MSG(0x00008, CTRUNC)
LINUX_MSG(0x00020, TRUNC)
LINUX_MSG(0x00040, DONTWAIT)
LINUX_MSG(0x00080, EOR)
LINUX_MSG(0x00100, WAITALL)
LINUX_MSG(0x04000, NOSIGNAL)
LINUX_MSG_IGNORED(0x00800, MSG_CONFIRM)
LINUX_MSG_IGNORED(0x08000, MSG_MORE)
LINUX_MSG_IGNORED(0x10000, MSG_WAITFORONE) /* recvmmsg reads it itself */
LINUX_MSG_IGNORED(0x40000000, MSG_CMSG_CLOEXEC)
#undef LINUX_MSG
#undef LINUX_MSG_IGNORED
