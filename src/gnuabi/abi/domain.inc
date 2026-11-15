/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LINUX_DOMAIN(guest, NAME): the guest's address families (Linux's asm-generic values) as the stack's SNX_AF_##NAME.
   AF_NETLINK and the others aren't here: socket() fails with EAFNOSUPPORT, which glibc's and musl's callers expect
   in containers that lack them */
LINUX_DOMAIN(0, UNSPEC)
LINUX_DOMAIN(1, UNIX)
LINUX_DOMAIN(2, INET)
LINUX_DOMAIN(10, INET6)
#undef LINUX_DOMAIN
