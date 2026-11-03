/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux's network ABI as the emulated stack's (common/network/net.h), for each runtime whose guest is Linux (the GNU
   ABI's socket calls, the Android bridge): the address families, the send and receive flags, the poll events, the
   socket options (the tables in this folder), and the AArch64 sockaddr_in and sockaddr_in6 as SnxSockAddr. The stack
   then gives each of them as the host's (net_internal.h). Only bytes: no guest memory */
#ifndef SNX_GNU_ABI_LINUX_NET_H
#define SNX_GNU_ABI_LINUX_NET_H
#include "common/network/net.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* a guest family as the stack's, -1 for one the stack has not */
static inline int gnu_linux_domain_in(uint32_t d) {
#define LINUX_DOMAIN(guest, name)                                                                                      \
    if (d == guest) return SNX_AF_##name;
#include "gnuabi/abi/linux_domain.inc"
    return -1;
}
static inline uint16_t gnu_linux_domain_out(int d) {
#define LINUX_DOMAIN(guest, name)                                                                                      \
    if (d == SNX_AF_##name) return guest;
#include "gnuabi/abi/linux_domain.inc"
    return 0;
}
static inline int gnu_linux_msg_in(uint64_t f) {
    int r = 0;
#define LINUX_MSG(guest, name)                                                                                         \
    if (f & guest) r |= SNX_MSG_##name;
#include "gnuabi/abi/linux_msgflag.inc"
    return r;
}
static inline uint32_t gnu_linux_msg_out(int f) {
    uint32_t r = 0;
#define LINUX_MSG(guest, name)                                                                                         \
    if (f & SNX_MSG_##name) r |= guest;
#include "gnuabi/abi/linux_msgflag.inc"
    return r;
}
static inline uint16_t gnu_linux_poll_in(uint32_t e) {
    uint16_t r = 0;
#define LINUX_POLL(guest, name)                                                                                        \
    if (e & guest) r |= SNX_POLL##name;
#include "gnuabi/abi/linux_poll.inc"
    return r;
}
static inline uint32_t gnu_linux_poll_out(uint16_t e) {
    uint32_t r = 0;
#define LINUX_POLL(guest, name)                                                                                        \
    if (e & SNX_POLL##name) r |= guest;
#include "gnuabi/abi/linux_poll.inc"
    return r;
}

/* how the guest lays an option's value out, when it differs from the stack's (linux_sockopt.inc) */
enum { GNU_LAYOUT_SAME, GNU_LAYOUT_MREQN, GNU_LAYOUT_IFADDR };
/* a guest level and option as the stack's: 0, or -SNX_ENOPROTOOPT */
static inline int gnu_linux_sockopt_in(uint64_t level, uint64_t name, int* l, int* n, int* layout) {
    int sl = -1;
#define LINUX_LEVEL(guest, nm)                                                                                         \
    if (level == guest) sl = SNX_SOL_##nm;
#include "gnuabi/abi/linux_sockopt.inc"
    if (sl < 0) return -SNX_ENOPROTOOPT;
    *l = sl;
#define LINUX_SOCKOPT(lv, guest, nm, lay)                                                                              \
    if (sl == SNX_SOL_##lv && name == guest) {                                                                         \
        *n = SNX_##nm;                                                                                                 \
        *layout = GNU_LAYOUT_##lay;                                                                                    \
        return 0;                                                                                                      \
    }
#include "gnuabi/abi/linux_sockopt.inc"
    return -SNX_ENOPROTOOPT;
}

/* the size of the guest's largest address of the stack's families (sockaddr_in6) */
enum { GNU_LINUX_SOCKADDR_MAX = 28, GNU_LINUX_SOCKADDR_UNNAMED = 2 };

/* the guest's address in b (len bytes, at most GNU_LINUX_SOCKADDR_MAX of them read) as the stack's: 0, 1 for an
   AF_UNIX one (its path the caller's), or -SNX_E* */
static inline int gnu_linux_addr_decode(const unsigned char* b, uint64_t len, SnxSockAddr* out) {
    *out = (SnxSockAddr){0};
    if (len < 2 || len > 128) return -SNX_EINVAL;
    int fam = gnu_linux_domain_in(snx_ld_le16(b));
    switch (fam) {
        case SNX_AF_UNSPEC: return 0;
        case SNX_AF_UNIX: return 1;
        case SNX_AF_INET:
            if (len < 16) return -SNX_EINVAL;
            out->family = SNX_AF_INET;
            memcpy(&out->port, b + 2, 2);
            memcpy(out->addr, b + 4, 4);
            return 0;
        case SNX_AF_INET6:
            if (len < 24) return -SNX_EINVAL;
            out->family = SNX_AF_INET6;
            memcpy(&out->port, b + 2, 2);
            memcpy(&out->flowinfo, b + 4, 4);
            memcpy(out->addr, b + 8, 16);
            if (len >= 28) out->scope_id = snx_ld_le32(b + 24);
            return 0;
    }
    return -SNX_EAFNOSUPPORT;
}
/* the stack's address as the guest's, into b: its size (0 for none, a connected stream socket's sender) */
static inline uint32_t gnu_linux_addr_encode(const SnxSockAddr* sa, unsigned char b[GNU_LINUX_SOCKADDR_MAX]) {
    memset(b, 0, GNU_LINUX_SOCKADDR_MAX);
    snx_st_le16(b, gnu_linux_domain_out(sa->family));
    if (sa->family == SNX_AF_INET6) {
        memcpy(b + 2, &sa->port, 2);
        memcpy(b + 4, &sa->flowinfo, 4);
        memcpy(b + 8, sa->addr, 16);
        snx_st_le32(b + 24, sa->scope_id);
        return 28;
    }
    if (sa->family == SNX_AF_INET) {
        memcpy(b + 2, &sa->port, 2);
        memcpy(b + 4, sa->addr, 4);
        return 16;
    }
    if (sa->family == SNX_AF_UNIX) return GNU_LINUX_SOCKADDR_UNNAMED; /* socketpair()'s ends: the family alone */
    return 0;
}
#endif
