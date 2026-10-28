/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/hle/service/sockets/bsd.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   bsd:u, bsd:a and bsd:s, Horizon's BSD sockets, on the emulated network stack (src/common/network). Requests are
   served on the guest thread that sends them, without its core (ipc_handle), so a blocking call blocks that thread
   only, as on the console where each of the SDK's sessions has a server thread. A socket command's reply is the
   call's result and its errno (Linux's numbering, the stack's too); the constants are FreeBSD's, translated with the
   tables in abi/.

   Eden stubs Read, Select and EventFd, answers GetSockOpt for SO_ERROR only, and shuts down the read side of a
   SocketExempt socket; those are real here. */
#include "nx/hle/services/network/network.h"
#include "nx/hle/hle.h"
#include "nx/memory/own.h"
#include "hle/services/network/bsd.swipc.inc"
#include <time.h>

enum {
    BSD_F_GETFD = 1,
    BSD_F_SETFD = 2,
    BSD_F_GETFL = 3,
    BSD_F_SETFL = 4,
    BSD_O_NONBLOCK = 0x4,      /* FreeBSD's */
    BSD_O_NONBLOCK_NX = 0x800, /* what the SDK and libnx send (Eden's NONBLOCK_NX) */
    BSD_SOCK_NONBLOCK = 0x20000000,
    BSD_SOCK_CLOEXEC = 0x10000000,
    BSD_EFD_SEMAPHORE = 0x1,
    BSD_EFD_NONBLOCK = 0x4,
    BSD_FIONBIO = 0x8004667E,
    BSD_FIONREAD = 0x4004667F,
    BSD_SOCKADDR_IN = 16,
    BSD_SOCKADDR_IN6 = 28,
};

/* ============================================================ the network option */
/* the Eden room the console joined with ldn=room (room.c): the game's datagram sockets are the room's, and reach
   the members' addresses whatever the network option says */
void network_set_room(SnxRoom* room) { g_hle.bsd.room = room; }
SnxRoom* network_room(void) { return g_hle.bsd.room; }
static int reachable(SnxSocket* s, const SnxSockAddr* a) { return snx_room_socket(s) || network_reachable(a); }
/* nifm asks often (some games every frame): the host's state is read again after two seconds. In a room the
   connection is the room's, its address and a /24, as Eden's nifm gives the room's address */
int network_interface(SnxNetInterface* out) {
    if (g_settings.airplane_mode) return -SNX_ENETDOWN;
    if (g_hle.bsd.room) {
        if (out) {
            memset(out, 0, sizeof *out);
            strcpy(out->name, "Eden room");
            snx_room_fake_ip(g_hle.bsd.room, out->ip);
            out->mask[0] = out->mask[1] = out->mask[2] = 255;
            out->wireless = 1;
        }
        return 0;
    }
    if (!g_settings.network) return -SNX_ENETDOWN;
    pm_lock(&g_host.bsd.iface_lk);
    time_t now = time(NULL);
    if (!g_hle.bsd.iface_when || now - g_hle.bsd.iface_when >= 2 || now < g_hle.bsd.iface_when) {
        g_hle.bsd.iface_have = !snx_net_primary_interface(&g_hle.bsd.iface);
        g_hle.bsd.iface_when = now;
    }
    if (g_hle.bsd.iface_have && out) *out = g_hle.bsd.iface;
    int r = g_hle.bsd.iface_have ? 0 : -SNX_ENETDOWN;
    pm_unlock(&g_host.bsd.iface_lk);
    return r;
}
int network_online(void) { return !network_interface(NULL); }
int network_reachable(const SnxSockAddr* a) {
    if ((g_settings.network && !g_settings.airplane_mode) || a->family == SNX_AF_UNSPEC) return 1;
    if (a->family == SNX_AF_INET) return a->addr[0] == 127;
    static const uint8_t lo6[16] = {[15] = 1};
    return a->family == SNX_AF_INET6 && !memcmp(a->addr, lo6, 16);
}
/* the address a bind to INADDR_ANY binds instead, while ldn gives this console one of its own on the LAN (ldn.c),
   and its subnet's broadcast address: two consoles on one host then keep their ports apart, and each still hears
   the broadcasts a game sends to the subnet (a Pia session's first messages). NULL clears it */
void bsd_set_bind_address(const u8* ip, const u8* bcast) {
    pm_lock(&g_host.bsd.bind_lk);
    g_hle.bsd.bind_set = ip != NULL;
    if (ip) memcpy(g_hle.bsd.bind_ip, ip, 4);
    if (ip) memcpy(g_hle.bsd.bind_bcast, bcast, 4);
    pm_unlock(&g_host.bsd.bind_lk);
}
/* 1 and the broadcast address when a was INADDR_ANY and became the console's address */
static int bind_address(SnxSockAddr* a, u8 bcast[4]) {
    static const u8 any[4];
    int r;
    if (a->family != SNX_AF_INET || memcmp(a->addr, any, 4)) return 0;
    pm_lock(&g_host.bsd.bind_lk);
    r = g_hle.bsd.bind_set;
    if (r) memcpy(a->addr, g_hle.bsd.bind_ip, 4);
    if (r) memcpy(bcast, g_hle.bsd.bind_bcast, 4);
    pm_unlock(&g_host.bsd.bind_lk);
    return r;
}
/* the broadcast companion of a datagram socket bound to the console's address: the same port on the broadcast
   address, shared with the other consoles on this host (SO_REUSEADDR), read only when the socket itself has nothing */
static SnxSocket* bcast_companion(SnxSocket* s, const u8 bcast[4]) {
    SnxSocket* c;
    SnxSockAddr a;
    int one = (int)snx_le32(1), r = snx_socket_name(s, &a); /* option values are the guest's (net.h) */
    if (r) return NULL;
    r = snx_socket_open(&c, SNX_AF_INET, SNX_SOCK_DGRAM, 0);
    if (r) return NULL;
    snx_socket_setsockopt(c, SNX_SOL_SOCKET, SNX_SO_REUSEADDR, &one, sizeof one);
    snx_socket_setsockopt(c, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one);
    memcpy(a.addr, bcast, 4);
    r = snx_socket_bind(c, &a);
    if (r) {
        WARN("bsd: the broadcast companion of a socket on %u.%u.%u.%u couldn't bind %u.%u.%u.%u: errno %d\n",
             g_hle.bsd.bind_ip[0], g_hle.bsd.bind_ip[1], g_hle.bsd.bind_ip[2], g_hle.bsd.bind_ip[3], bcast[0], bcast[1], bcast[2], bcast[3], -r);
        snx_socket_unref(c);
        return NULL;
    }
    snx_socket_set_nonblocking(c, 1);
    return c;
}
static void net_log(const char* m) { LOG("%s", m); }
static void net_warn(const char* m) { WARN("%s", m); }
void network_init(void) {
    snx_net_set_log(net_log);
    snx_net_set_warn(net_warn);
    if (snx_net_init()) WARN("bsd: the host's network stack didn't start, sockets will fail\n");
}

/* ============================================================ translations (abi/) */
static int domain_in(u32 d) {
#define BSD_DOMAIN(guest, name)                                                                                        \
    if (d == guest) return SNX_AF_##name;
#include "nx/hle/services/network/abi/bsd_domain.inc"
    return -1;
}
static u32 domain_out(int d) {
#define BSD_DOMAIN(guest, name)                                                                                        \
    if (d == SNX_AF_##name) return guest;
#include "nx/hle/services/network/abi/bsd_domain.inc"
    return 0;
}
static int msg_in(u32 f) {
    int r = 0;
#define BSD_MSG(guest, name)                                                                                           \
    if (f & guest) r |= SNX_MSG_##name;
#include "nx/hle/services/network/abi/bsd_msgflag.inc"
    return r;
}
static uint16_t poll_in(u16 e) {
    uint16_t r = 0;
#define BSD_POLL(guest, name)                                                                                          \
    if (e & guest) r |= SNX_POLL##name;
#include "nx/hle/services/network/abi/bsd_poll.inc"
    return r;
}
static u16 poll_out(uint16_t e) {
    u16 r = 0;
#define BSD_POLL(guest, name)                                                                                          \
    if (e & SNX_POLL##name) r |= guest;
#include "nx/hle/services/network/abi/bsd_poll.inc"
    if (e & SNX_POLLWRNORM) r |= 0x0004;
    return r;
}
/* 1 and the stack's level and name, 0 for an option to accept and ignore, -1 for one Horizon doesn't have */
static int sockopt_in(u32 level, u32 name, int* l, int* n) {
    int sl = -1;
#define BSD_LEVEL(guest, nm)                                                                                           \
    if (level == guest) sl = SNX_SOL_##nm;
#include "nx/hle/services/network/abi/bsd_sockopt.inc"
    if (sl < 0) return -1;
    *l = sl;
#define BSD_SOCKOPT(lv, guest, nm)                                                                                     \
    if (sl == SNX_SOL_##lv && name == guest) {                                                                         \
        *n = SNX_##nm;                                                                                                 \
        return 1;                                                                                                      \
    }
#define BSD_SOCKOPT_IGNORED(lv, guest, nm)                                                                             \
    if (sl == SNX_SOL_##lv && name == guest) return 0;
#include "nx/hle/services/network/abi/bsd_sockopt.inc"
    return -1;
}

/* guest sockaddr_in {u8 len, u8 family, be16 port, in_addr, zero[8]}, sockaddr_in6 {len, family, port, u32
   flowinfo, in6_addr, u32 scope} */
static int addr_in(u64 a, u64 n, SnxSockAddr* out) {
    memset(out, 0, sizeof *out);
    if (!a || n < 8) return -SNX_EINVAL;
    const u8* p = GP(a);
    int fam = p[1] ? domain_in(p[1]) : SNX_AF_INET;
    if (fam == SNX_AF_INET) {
        out->family = SNX_AF_INET;
        memcpy(&out->port, p + 2, 2);
        memcpy(out->addr, p + 4, 4);
        return 0;
    }
    if (fam == SNX_AF_INET6 && n >= 24) {
        out->family = SNX_AF_INET6;
        memcpy(&out->port, p + 2, 2);
        memcpy(&out->flowinfo, p + 4, 4);
        memcpy(out->addr, p + 8, 16);
        if (n >= 28) out->scope_id = snx_ld_le32(p + 24); /* a number, where the port and flowinfo are bytes */
        return 0;
    }
    return -SNX_EAFNOSUPPORT;
}
/* writes what fits of the address, and returns the length written (the addrlen the guest gets back), as a BSD stack
   answers getsockname, getpeername, accept and recvfrom */
static u32 addr_out(const SnxSockAddr* a, u64 dst, u64 cap) {
    u8 p[BSD_SOCKADDR_IN6];
    u32 n = a->family == SNX_AF_INET6 ? BSD_SOCKADDR_IN6 : BSD_SOCKADDR_IN;
    memset(p, 0, sizeof p);
    p[0] = (u8)n;
    p[1] = (u8)domain_out(a->family ? a->family : SNX_AF_INET);
    memcpy(p + 2, &a->port, 2);
    if (n == BSD_SOCKADDR_IN6) {
        memcpy(p + 4, &a->flowinfo, 4);
        memcpy(p + 8, a->addr, 16);
        snx_st_le32(p + 24, a->scope_id);
    } else
        memcpy(p + 4, a->addr, 4);
    if (!dst) return 0;
    if (cap < n) n = (u32)cap;
    memcpy(GP(dst), p, n);
    return n;
}

/* an address for the log, at level 2: "127.0.0.1:1234", "[v6]:1234" or "?" */
static const char* addr_text(const SnxSockAddr* a, char* buf, size_t n) {
    u16 port = snx_ld_be16(&a->port);
    if (a->family == SNX_AF_INET)
        snprintf(buf, n, "%u.%u.%u.%u:%u", a->addr[0], a->addr[1], a->addr[2], a->addr[3], port);
    else if (a->family == SNX_AF_INET6)
        snprintf(buf, n, "[v6]:%u", port);
    else
        snprintf(buf, n, "?");
    return buf;
}

/* ============================================================ descriptors */
static SnxSocket* fd_get(Bsd* b, i32 fd, BsdFd* copy) {
    SnxSocket* s = NULL;
    if (fd < 0 || fd >= BSD_MAX_FD) return NULL;
    pm_lock(&b->lk);
    if (b->fd[fd].s) {
        s = snx_socket_ref(b->fd[fd].s);
        if (copy) *copy = b->fd[fd];
    }
    pm_unlock(&b->lk);
    return s;
}
/* takes the reference s carries. -SNX_EMFILE when the table is full */
static int fd_add(Bsd* b, SnxSocket* s, int flags, int exempt) {
    pm_lock(&b->lk);
    for (int i = 0; i < BSD_MAX_FD; i++)
        if (!b->fd[i].s) {
            b->fd[i].s = s;
            b->fd[i].flags = flags;
            b->fd[i].exempt = exempt;
            pm_unlock(&b->lk);
            return i;
        }
    pm_unlock(&b->lk);
    snx_socket_unref(s);
    WARN_LIMIT(4, "bsd: no free descriptor\n");
    return -SNX_EMFILE;
}
/* The session a process registered (RegisterClient), whose table holds the descriptors it hands ssl. Not referenced:
   bsd_destroy clears it under the lock, so a lookup that holds the lock sees the session whole */
SnxSocket* bsd_client_socket(u64 pid, i32 fd) {
    SnxSocket* s = NULL;
    pm_lock(&g_host.bsd.client_lk);
    if (g_hle.bsd.client && g_hle.bsd.client_pid == pid) s = fd_get(g_hle.bsd.client->data, fd, NULL);
    pm_unlock(&g_host.bsd.client_lk);
    return s;
}
static void bsd_destroy(SvcObj* o) {
    Bsd* b = o->data;
    pm_lock(&g_host.bsd.client_lk);
    if (g_hle.bsd.client == o) g_hle.bsd.client = NULL;
    pm_unlock(&g_host.bsd.client_lk);
    for (int i = 0; i < BSD_MAX_FD; i++) {
        snx_socket_unref(b->fd[i].s);
        snx_socket_unref(b->fd[i].bcast);
    }
}
/* the broadcast companion of descriptor fd, with a reference for the caller, or NULL */
static SnxSocket* fd_get_bcast(Bsd* b, i32 fd) {
    SnxSocket* c = NULL;
    if (fd < 0 || fd >= BSD_MAX_FD) return NULL;
    pm_lock(&b->lk);
    if (b->fd[fd].s && b->fd[fd].bcast) c = snx_socket_ref(b->fd[fd].bcast);
    pm_unlock(&b->lk);
    return c;
}

/* ============================================================ replies */
/* the BSD result a socket command answers: -1 and the errno, or the result and 0 */
static int reply(int64_t r, i32* ret, u32* bsd_errno) {
    *ret = r < 0 ? -1 : (i32)r;
    *bsd_errno = r < 0 ? (u32)-r : 0;
    R_SUCCEED();
}
/* and an address's or an option's length, 0 on failure */
static int reply_len(int64_t r, u32 len, i32* ret, u32* bsd_errno, u32* out_len) {
    *out_len = r < 0 ? 0 : len;
    return reply(r, ret, bsd_errno);
}
/* a command bsd doesn't have answers EOPNOTSUPP, in the result a socket command starts with */
static int bsd_unknown(SvcObj* self, IpcCtx* c) {
    if (ipc_warn_once(self->name, c->cmd)) WARN("  [not implemented] %s cmd %u -> EOPNOTSUPP\n", self->name, c->cmd);
    out32(c, (u32)-1);
    out32(c, SNX_EOPNOTSUPP);
    R_SUCCEED();
}
#define WITH_SOCKET(fdarg)                                                                                             \
    BsdFd d;                                                                                                           \
    SnxSocket* s = fd_get(b, (i32)(fdarg), &d);                                                                        \
    if (!s) return reply(-SNX_EBADF, ret, bsd_errno)

/* ============================================================ commands */
/* The SDK registers once per process, on the session its socket calls go through (a later Initialize after Finalize
   registers a new one, which takes over). The transfer memory is the console's buffer space: the host's stack needs
   none */
static int bsd_register(SvcObj* self, u64 pid) {
    pm_lock(&g_host.bsd.client_lk);
    g_hle.bsd.client = self;
    g_hle.bsd.client_pid = pid;
    pm_unlock(&g_host.bsd.client_lk);
    R_SUCCEED();
}
static int nn_socket_sf_IClient_RegisterClient(SvcObj* self, const nn_socket_sf_LibraryConfigData* config, u64 pid,
                                               u64 transfer_memory_size, u32 transfer_memory, u32* bsd_errno) {
    return bsd_register(self, pid);
}
static int nn_socket_sf_IClient_RegisterClientShared(SvcObj* self, const nn_socket_sf_LibraryConfigData* config,
                                                     u64 pid, u64 work_buffer_size, u32* bsd_errno) {
    return bsd_register(self, pid);
}
static int bsd_socket(Bsd* b, i32 gdom_in, i32 gtype_in, i32 proto_in, int exempt, i32* ret, u32* bsd_errno) {
    u32 gdom = (u32)gdom_in, gtype = (u32)gtype_in, proto = (u32)proto_in;
    int dom = domain_in(gdom), type = (int)(gtype & ~(u32)(BSD_SOCK_NONBLOCK | BSD_SOCK_CLOEXEC));
    if (dom < 0) return reply(-SNX_EAFNOSUPPORT, ret, bsd_errno);
    /* bsd:u: no raw or seqpacket sockets, except raw ICMP (Eden) */
    if (!b->system && (type == SNX_SOCK_SEQPACKET || type == SNX_SOCK_RAW) &&
        !(type == SNX_SOCK_RAW && dom == SNX_AF_INET && proto == SNX_IPPROTO_ICMP))
        return reply(-SNX_EINVAL, ret, bsd_errno);
    SnxSocket* s;
    int r = g_hle.bsd.room && dom == SNX_AF_INET && type == SNX_SOCK_DGRAM && (proto == 0 || proto == SNX_IPPROTO_UDP)
                ? snx_room_socket_open(g_hle.bsd.room, &s, dom, type, (int)proto)
                : snx_socket_open(&s, dom, type, (int)proto);
    if (r) {
        LOG("bsd: Socket(%u, %u, %u) failed: errno %d\n", gdom, gtype, proto, -r);
        return reply(r, ret, bsd_errno);
    }
    if (gtype & BSD_SOCK_NONBLOCK) snx_socket_set_nonblocking(s, 1);
    r = fd_add(b, s, (gtype & BSD_SOCK_NONBLOCK) ? BSD_O_NONBLOCK_NX : 0, exempt);
    LOGV("bsd: Socket(%u, %u, %u) = %d\n", gdom, gtype, proto, r);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Socket(SvcObj* self, i32 domain, i32 type, i32 protocol, i32* ret, u32* bsd_errno) {
    return bsd_socket(self->data, domain, type, protocol, 0, ret, bsd_errno);
}
static int nn_socket_sf_IClient_SocketExempt(SvcObj* self, i32 domain, i32 type, i32 protocol, i32* ret,
                                             u32* bsd_errno) {
    return bsd_socket(self->data, domain, type, protocol, 1, ret, bsd_errno);
}
/* no devices */
static int nn_socket_sf_IClient_Open(SvcObj* self, u32 flags, IpcBuf path, i32* ret, u32* bsd_errno) {
    return reply(-SNX_ENOENT, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Select(SvcObj* self, i32 nfds, const nn_socket_sf_SelectTimeval* timeout,
                                       IpcBuf readfds_in, IpcBuf writefds_in, IpcBuf errorfds_in, IpcBuf readfds_out,
                                       IpcBuf writefds_out, IpcBuf errorfds_out, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    IpcBuf in[3] = {readfds_in, writefds_in, errorfds_in}, out[3] = {readfds_out, writefds_out, errorfds_out};
    if (nfds < 0 || nfds > BSD_MAX_FD) return reply(-SNX_EINVAL, ret, bsd_errno);
    SnxPollEntry e[BSD_MAX_FD * 2];
    int which[BSD_MAX_FD], owner[BSD_MAX_FD], n = 0, extra = 0;
    int64_t r = 0;
    for (i32 fd = 0; fd < nfds; fd++) {
        uint16_t ev = 0;
        for (int k = 0; k < 3; k++)
            if (in[k].addr && (u64)fd / 8 < in[k].size && (((u8*)GP(in[k].addr))[fd / 8] >> (fd % 8) & 1))
                ev |= k == 0 ? SNX_POLLIN : k == 1 ? SNX_POLLOUT : SNX_POLLPRI;
        if (!ev) continue;
        SnxSocket* s = fd_get(b, fd, NULL);
        if (!s) {
            r = -SNX_EBADF;
            break;
        }
        e[n].socket = s;
        e[n].host_fd = -1;
        e[n].events = ev;
        which[n++] = fd;
    }
    /* the broadcast companions, after the descriptors' entries; a ready companion makes its descriptor readable */
    for (int i = 0; !r && i < n; i++) {
        SnxSocket* c = (e[i].events & SNX_POLLIN) ? fd_get_bcast(b, which[i]) : NULL;
        if (!c) continue;
        e[n + extra].socket = c;
        e[n + extra].host_fd = -1;
        e[n + extra].events = SNX_POLLIN;
        owner[extra++] = i;
    }
    if (!r) {
        int ms = -1;
        if (!timeout->is_null) {
            u64 t = (u64)timeout->sec * 1000 + ((u64)timeout->usec + 999) / 1000;
            ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (int)t;
        }
        r = snx_net_poll(e, (size_t)(n + extra), ms);
    }
    for (int k = 0; k < extra; k++) {
        if (r > 0 && (e[n + k].revents & SNX_POLLIN)) e[owner[k]].revents |= SNX_POLLIN;
        snx_socket_unref(e[n + k].socket);
    }
    if (r >= 0) {
        u8 sets[3][BSD_MAX_FD / 8];
        memset(sets, 0, sizeof sets);
        r = 0;
        for (int i = 0; i < n; i++) {
            uint16_t ev = e[i].revents;
            int fd = which[i];
            int hit[3] = {(e[i].events & SNX_POLLIN) && (ev & (SNX_POLLIN | SNX_POLLHUP | SNX_POLLERR)),
                          (e[i].events & SNX_POLLOUT) && (ev & (SNX_POLLOUT | SNX_POLLHUP | SNX_POLLERR)),
                          (e[i].events & SNX_POLLPRI) && (ev & SNX_POLLPRI)};
            for (int k = 0; k < 3; k++)
                if (hit[k]) {
                    sets[k][fd / 8] |= (u8)(1 << (fd % 8));
                    r++;
                }
        }
        for (int k = 0; k < 3; k++)
            if (out[k].addr) {
                memset(GP(out[k].addr), 0, out[k].size);
                memcpy(GP(out[k].addr), sets[k], out[k].size < sizeof sets[k] ? out[k].size : sizeof sets[k]);
            }
    }
    for (int i = 0; i < n; i++) snx_socket_unref(e[i].socket);
    return reply(r, ret, bsd_errno);
}
/* the stack has no sysctl tree: every name is unsupported, with nothing in the old value */
static int nn_socket_sf_IClient_Sysctl(SvcObj* self, IpcBuf name, IpcBuf old_value, IpcBuf new_value, i32* ret,
                                       u32* bsd_errno, u32* old_length) {
    LOG_LIMIT(4, "bsd: Sysctl -> EOPNOTSUPP\n");
    return reply(-SNX_EOPNOTSUPP, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Poll(SvcObj* self, i32 nfds, i32 timeout, IpcBuf fds_in, IpcBuf fds_out, i32* ret,
                                     u32* bsd_errno) {
    Bsd* b = self->data;
    u64 ia = fds_in.addr, is = fds_in.size, oa = fds_out.addr, os = fds_out.size;
    /* Eden: no entries is -1 with errno 0 */
    if (!nfds) {
        *ret = -1;
        R_SUCCEED();
    }
    if (nfds < 0) return reply(-SNX_EINVAL, ret, bsd_errno);
    if (nfds > 4096 || !ia || is < (u64)nfds * 8 || (oa && os < (u64)nfds * 8))
        return reply(-SNX_EINVAL, ret, bsd_errno);
    /* an entry per descriptor, then one per broadcast companion, whose readiness its descriptor's entry takes */
    SnxPollEntry* e = calloc((size_t)nfds * 2, sizeof *e);
    i32* owner = calloc((size_t)nfds, sizeof *owner);
    if (!e || !owner) {
        free(e);
        free(owner);
        return reply(-SNX_ENOMEM, ret, bsd_errno);
    }
    const u8* in = GP(ia);
    int bad = 0;
    i32 extra = nfds;
    for (i32 i = 0; i < nfds; i++) {
        i32 fd;
        u16 ev;
        fd = (i32)snx_ld_le32(in + i * 8);
        ev = snx_ld_le16(in + i * 8 + 4);
        e[i].host_fd = -1;
        e[i].events = poll_in(ev);
        if (fd < 0) continue; /* ignored, as POSIX says */
        e[i].socket = fd_get(b, fd, NULL);
        if (!e[i].socket) bad++;
        if (e[i].socket && (e[i].events & SNX_POLLIN)) {
            SnxSocket* c = fd_get_bcast(b, fd);
            if (c) {
                e[extra].socket = c;
                e[extra].host_fd = -1;
                e[extra].events = SNX_POLLIN;
                owner[extra - nfds] = i;
                extra++;
            }
        }
    }
    int64_t r = snx_net_poll(e, (size_t)extra, bad ? 0 : timeout);
    u8* out = oa ? GP(oa) : NULL;
    if (r >= 0) r += bad;
    for (i32 k = nfds; k < extra; k++) { /* a companion's hit is its descriptor's, counted once */
        if (r > 0 && (e[k].revents & SNX_POLLIN)) {
            if (e[owner[k - nfds]].revents) r--;
            e[owner[k - nfds]].revents |= SNX_POLLIN;
        }
        snx_socket_unref(e[k].socket);
    }
    for (i32 i = 0; i < nfds; i++) {
        i32 fd = (i32)snx_ld_le32(in + i * 8);
        u16 rev = r >= 0 ? poll_out(e[i].revents) : 0;
        if (r >= 0 && fd >= 0 && !e[i].socket) rev = 0x0020; /* POLLNVAL */
        if (out) {
            memmove(out + i * 8, in + i * 8, 6);
            snx_st_le16(out + i * 8 + 6, rev);
        }
        snx_socket_unref(e[i].socket);
    }
    free(e);
    free(owner);
    return reply(r, ret, bsd_errno);
}
/* a receive on a socket with a broadcast companion: whichever has a datagram, the socket first; a blocking one
   waits for either within the socket's SO_RCVTIMEO */
static int64_t recv_either(SnxSocket* s, SnxSocket* c, void* data, size_t n, int flags, SnxSockAddr* from) {
    int64_t r;
    if (!snx_socket_nonblocking(s) && !(flags & SNX_MSG_DONTWAIT)) {
        SnxPollEntry e[2];
        SnxTimeval tv = {0, 0};
        size_t size = sizeof tv;
        int ms = -1;
        if (!snx_socket_getsockopt(s, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, &size)) { /* the guest's order */
            tv.sec = (int64_t)snx_le64((uint64_t)tv.sec);
            tv.usec = (int64_t)snx_le64((uint64_t)tv.usec);
            if (tv.sec || tv.usec) ms = (int)(tv.sec * 1000 + tv.usec / 1000);
        }
        e[0].socket = s;
        e[1].socket = c;
        e[0].host_fd = e[1].host_fd = -1;
        e[0].events = e[1].events = SNX_POLLIN;
        e[0].revents = e[1].revents = 0;
        r = snx_net_poll(e, 2, ms);
        if (r < 0) return r;
        if (r == 0) return -SNX_EAGAIN;
        if (!e[0].revents) return snx_socket_recv(c, data, n, flags | SNX_MSG_DONTWAIT, from, NULL);
    }
    r = snx_socket_recv(s, data, n, flags | SNX_MSG_DONTWAIT, from, NULL);
    if (r == -SNX_EAGAIN) r = snx_socket_recv(c, data, n, flags | SNX_MSG_DONTWAIT, from, NULL);
    return r;
}
static int64_t do_recv(Bsd* b, IpcBuf message, i32 fd, u32 gflags, IpcBuf addr, u32* addrlen) {
    u64 a = message.addr, n = message.size;
    SnxSocket* s = fd_get(b, fd, NULL);
    if (!s) return -SNX_EBADF;
    SnxSocket* c = fd_get_bcast(b, fd);
    SnxSockAddr from = {0};
    int64_t r;
    /* the kernel writes the buffer, and can't wait on memory the GPU holds nor fault on a page the write tracking
       protects */
    if (n && a) {
        own_acquire(a, n);
        plat_ww_open(GP(a), n);
    }
    if (!n)
        r = 0; /* Eden: nothing to receive into, and a datagram stays queued */
    else if (!a)
        r = -SNX_EFAULT;
    else if (c)
        r = recv_either(s, c, GP(a), n, msg_in(gflags), &from);
    else
        r = snx_socket_recv(s, GP(a), n, msg_in(gflags), addrlen ? &from : NULL, NULL);
    snx_socket_unref(c);
    if (r > 0 && from.family && g_settings.log_level >= 2) {
        char t[64];
        LOGV("bsd: RecvFrom(%d) %lld bytes from %s\n", fd, (long long)r, addr_text(&from, t, sizeof t));
    }
    /* a connected stream socket names no sender (Eden clears the address for connection based sockets) */
    if (r >= 0 && addrlen) {
        if (from.family && snx_socket_type(s) != SNX_SOCK_STREAM)
            *addrlen = addr_out(&from, addr.addr, addr.size);
        else
            *addrlen = 0;
    }
    snx_socket_unref(s);
    return r;
}
static int64_t do_send(Bsd* b, IpcBuf message, i32 fd, u32 gflags, const SnxSockAddr* to) {
    u64 a = message.addr, n = message.size;
    SnxSocket* s = fd_get(b, fd, NULL);
    if (!s) return -SNX_EBADF;
    int64_t r;
    if (to && !reachable(s, to))
        r = -SNX_ENETUNREACH;
    else if (!a && n)
        r = -SNX_EFAULT;
    else {
        if (a) own_acquire(a, n); /* the kernel reads the buffer, and can't wait on memory the GPU holds */
        r = snx_socket_send(s, a ? GP(a) : NULL, n, msg_in(gflags), to);
    }
    if (to && g_settings.log_level >= 2) {
        char t[64];
        LOGV("bsd: SendTo(%d) %llu bytes to %s = %lld\n", fd, (unsigned long long)n, addr_text(to, t, sizeof t),
             (long long)r);
    }
    snx_socket_unref(s);
    return r;
}
static const IpcBuf no_buffer = {0, 0, 0};
static int nn_socket_sf_IClient_Recv(SvcObj* self, i32 fd, i32 flags, IpcBuf message, i32* ret, u32* bsd_errno) {
    return reply(do_recv(self->data, message, fd, (u32)flags, no_buffer, NULL), ret, bsd_errno);
}
static int nn_socket_sf_IClient_Read(SvcObj* self, i32 fd, IpcBuf message, i32* ret, u32* bsd_errno) {
    return reply(do_recv(self->data, message, fd, 0, no_buffer, NULL), ret, bsd_errno);
}
static int nn_socket_sf_IClient_RecvFrom(SvcObj* self, i32 fd, i32 flags, IpcBuf message, IpcBuf addr, i32* ret,
                                         u32* bsd_errno, u32* addrlen) {
    u32 alen = 0;
    int64_t r = do_recv(self->data, message, fd, (u32)flags, addr, &alen);
    return reply_len(r, alen, ret, bsd_errno, addrlen);
}
static int nn_socket_sf_IClient_Send(SvcObj* self, i32 fd, i32 flags, IpcBuf message, i32* ret, u32* bsd_errno) {
    return reply(do_send(self->data, message, fd, (u32)flags, NULL), ret, bsd_errno);
}
static int nn_socket_sf_IClient_Write(SvcObj* self, i32 fd, IpcBuf message, i32* ret, u32* bsd_errno) {
    return reply(do_send(self->data, message, fd, 0, NULL), ret, bsd_errno);
}
static int nn_socket_sf_IClient_SendTo(SvcObj* self, i32 fd, i32 flags, IpcBuf message, IpcBuf addr, i32* ret,
                                       u32* bsd_errno) {
    SnxSockAddr to;
    if (!addr.addr || !addr.size) return reply(do_send(self->data, message, fd, (u32)flags, NULL), ret, bsd_errno);
    int r = addr_in(addr.addr, addr.size, &to);
    return reply(r ? r : do_send(self->data, message, fd, (u32)flags, &to), ret, bsd_errno);
}
static int nn_socket_sf_IClient_Accept(SvcObj* self, i32 fd, IpcBuf addr, i32* ret, u32* bsd_errno, u32* addrlen) {
    Bsd* b = self->data;
    BsdFd d;
    SnxSocket* s = fd_get(b, fd, &d);
    if (!s) return reply_len(-SNX_EBADF, 0, ret, bsd_errno, addrlen);
    SnxSocket* n;
    SnxSockAddr peer;
    int r = snx_socket_accept(s, &n, &peer, snx_socket_nonblocking(s)); /* FreeBSD inherits O_NONBLOCK */
    snx_socket_unref(s);
    if (r) return reply_len(r, 0, ret, bsd_errno, addrlen);
    r = fd_add(b, n, d.flags & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX), 0);
    return reply_len(r, r < 0 ? 0 : addr_out(&peer, addr.addr, addr.size), ret, bsd_errno, addrlen);
}
/* Bind, Connect */
static int bind_connect(Bsd* b, i32 fd, IpcBuf addr, int connect, i32* ret, u32* bsd_errno) {
    SnxSockAddr sa;
    int r = addr_in(addr.addr, addr.size, &sa);
    if (r) return reply(r, ret, bsd_errno);
    WITH_SOCKET(fd);
    (void)d;
    if (!connect) {
        u8 bcast[4];
        int pinned = bind_address(&sa, bcast);
        r = snx_socket_bind(s, &sa);
        if (g_settings.log_level >= 2) {
            char t[64];
            LOGV("bsd: Bind(%d) %s = %d\n", fd, addr_text(&sa, t, sizeof t), r);
        }
        if (!r && pinned && snx_socket_type(s) == SNX_SOCK_DGRAM) {
            SnxSocket* c = bcast_companion(s, bcast);
            pm_lock(&b->lk);
            if (c && b->fd[fd].s == s && !b->fd[fd].bcast) {
                b->fd[fd].bcast = c;
                c = NULL;
            }
            pm_unlock(&b->lk);
            snx_socket_unref(c);
        }
    } else if (!reachable(s, &sa))
        r = -SNX_ENETUNREACH;
    else {
        r = snx_socket_connect(s, &sa);
        if (r == -SNX_EISCONN) r = 0; /* Eden: some games connect twice */
    }
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Bind(SvcObj* self, i32 fd, IpcBuf addr, i32* ret, u32* bsd_errno) {
    return bind_connect(self->data, fd, addr, 0, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Connect(SvcObj* self, i32 fd, IpcBuf addr, i32* ret, u32* bsd_errno) {
    return bind_connect(self->data, fd, addr, 1, ret, bsd_errno);
}
/* GetPeerName, GetSockName */
static int bsd_name(Bsd* b, i32 fd, IpcBuf addr, int peer, i32* ret, u32* bsd_errno, u32* addrlen) {
    SnxSocket* s = fd_get(b, fd, NULL);
    if (!s) return reply_len(-SNX_EBADF, 0, ret, bsd_errno, addrlen);
    SnxSockAddr sa;
    int r = peer ? snx_socket_peer(s, &sa) : snx_socket_name(s, &sa);
    snx_socket_unref(s);
    return reply_len(r, r ? 0 : addr_out(&sa, addr.addr, addr.size), ret, bsd_errno, addrlen);
}
static int nn_socket_sf_IClient_GetPeerName(SvcObj* self, i32 fd, IpcBuf addr, i32* ret, u32* bsd_errno, u32* addrlen) {
    return bsd_name(self->data, fd, addr, 1, ret, bsd_errno, addrlen);
}
static int nn_socket_sf_IClient_GetSockName(SvcObj* self, i32 fd, IpcBuf addr, i32* ret, u32* bsd_errno, u32* addrlen) {
    return bsd_name(self->data, fd, addr, 0, ret, bsd_errno, addrlen);
}
static int nn_socket_sf_IClient_GetSockOpt(SvcObj* self, i32 fd, i32 level_in, i32 option_name, IpcBuf option_value,
                                           i32* ret, u32* bsd_errno, u32* option_len) {
    u32 glevel = (u32)level_in, gname = (u32)option_name;
    SnxSocket* s = fd_get(self->data, fd, NULL);
    if (!s) return reply_len(-SNX_EBADF, 0, ret, bsd_errno, option_len);
    int level, name, k = sockopt_in(glevel, gname, &level, &name), r = 0;
    u8 v[32];
    size_t len = sizeof v;
    memset(v, 0, sizeof v);
    if (k < 0)
        r = -SNX_ENOPROTOOPT;
    else if (k == 0)
        len = 4; /* an option the stack ignores reads as 0 */
    else
        r = snx_socket_getsockopt(s, level, name, v, &len);
    snx_socket_unref(s);
    if (k < 0) WARN_LIMIT(8, "bsd: GetSockOpt of unknown option level %#x name %#x\n", glevel, gname);
    if (!r && option_value.addr) memcpy(GP(option_value.addr), v, option_value.size < len ? option_value.size : len);
    return reply_len(r, (u32)len, ret, bsd_errno, option_len);
}
static int nn_socket_sf_IClient_SetSockOpt(SvcObj* self, i32 fd, i32 level_in, i32 option_name, IpcBuf option_value,
                                           i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    u32 glevel = (u32)level_in, gname = (u32)option_name;
    u64 a = option_value.addr, n = option_value.size;
    WITH_SOCKET(fd);
    (void)d;
    int level, name, k = sockopt_in(glevel, gname, &level, &name), r = 0;
    if (k > 0) {
        u8 v[32];
        i64 sec;
        memset(v, 0, sizeof v);
        if (a) memcpy(v, GP(a), n < sizeof v ? n : sizeof v);
        sec = (i64)snx_ld_le64(v); /* the value stays in the guest's order: the stack converts it (net.h) */
        /* FreeBSD's sosetopt: a timeout before zero is out of range, where Linux (and the stack) take it as expired */
        if (level == SNX_SOL_SOCKET && (name == SNX_SO_RCVTIMEO || name == SNX_SO_SNDTIMEO) && n >= 16 && sec < 0)
            r = -SNX_EDOM;
        else
            r = snx_socket_setsockopt(s, level, name, v, n);
        /* the host refusing an option the console has (a buffer size out of its range, SO_DEBUG without
           privileges) isn't the game's fault: it gets what the console would answer */
        if (r == -SNX_ENOPROTOOPT || r == -SNX_EPERM || r == -SNX_EACCES) {
            LOG_LIMIT(8, "bsd: the host refused option level %#x name %#x (errno %d): the game is told it is set\n",
                      glevel, gname, -r);
            r = 0;
        }
    } else if (k < 0)
        WARN_LIMIT(8, "bsd: SetSockOpt of unknown option level %#x name %#x: not set, and the game is told it is\n",
                   glevel, gname);
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Listen(SvcObj* self, i32 fd, i32 backlog, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    WITH_SOCKET(fd);
    (void)d;
    int r = snx_socket_listen(s, backlog);
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Fcntl(SvcObj* self, i32 fd, i32 cmd, i32 arg, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    WITH_SOCKET(fd);
    int64_t r;
    switch (cmd) {
        case BSD_F_GETFL:
            r = d.flags |
                (snx_socket_nonblocking(s) && !(d.flags & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX)) ? BSD_O_NONBLOCK_NX
                                                                                                : 0);
            break;
        case BSD_F_SETFL:
            snx_socket_set_nonblocking(s, (arg & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX)) != 0);
            pm_lock(&b->lk);
            if (b->fd[fd].s == s) b->fd[fd].flags = arg;
            pm_unlock(&b->lk);
            r = 0;
            break;
        case BSD_F_GETFD:
        case BSD_F_SETFD: r = 0; break;
        default: r = -SNX_EINVAL; break;
    }
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Ioctl(SvcObj* self, i32 fd, u32 request, i32 bufcount, IpcBuf in0, IpcBuf in1,
                                      IpcBuf in2, IpcBuf in3, IpcBuf out0, IpcBuf out1, IpcBuf out2, IpcBuf out3,
                                      i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    u64 ia = in0.addr, is = in0.size, oa = out0.addr, os = out0.size;
    WITH_SOCKET(fd);
    (void)d;
    int64_t r = 0;
    if (request == BSD_FIONBIO) {
        u64 src = ia ? ia : oa, size = ia ? is : os;
        r = src && size >= 4 ? snx_socket_set_nonblocking(s, gr32(src) != 0) : -SNX_EFAULT;
    } else if (request == BSD_FIONREAD) {
        int bytes = 0;
        r = snx_socket_readable_bytes(s, &bytes);
        if (!r && oa && os >= 4)
            gw32(oa, (u32)bytes);
        else if (!r && ia && is >= 4)
            gw32(ia, (u32)bytes); /* an inout buffer */
    } else {
        WARN_LIMIT(4, "bsd: Ioctl %#x is not implemented\n", request);
        r = -SNX_EINVAL;
    }
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Shutdown(SvcObj* self, i32 fd, i32 how, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    WITH_SOCKET(fd);
    (void)d;
    int r = snx_socket_shutdown(s, how);
    snx_socket_unref(s);
    return reply(r, ret, bsd_errno);
}
static int nn_socket_sf_IClient_ShutdownAllSockets(SvcObj* self, i32 how, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    for (int i = 0; i < BSD_MAX_FD; i++) {
        BsdFd d;
        SnxSocket* s = fd_get(b, i, &d);
        if (!s) continue;
        if (!d.exempt) snx_socket_shutdown(s, how);
        snx_socket_unref(s);
    }
    return reply(0, ret, bsd_errno);
}
static int nn_socket_sf_IClient_Close(SvcObj* self, i32 fd, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    SnxSocket *s = NULL, *c = NULL;
    if (fd >= 0 && fd < BSD_MAX_FD) {
        pm_lock(&b->lk);
        s = b->fd[fd].s;
        c = b->fd[fd].bcast;
        memset(&b->fd[fd], 0, sizeof b->fd[fd]);
        pm_unlock(&b->lk);
    }
    if (!s) return reply(-SNX_EBADF, ret, bsd_errno);
    snx_socket_unref(c);
    snx_socket_unref(s);
    LOGV("bsd: Close(%d)\n", fd);
    return reply(0, ret, bsd_errno);
}
/* bsd:s only (Eden) */
static int nn_socket_sf_IClient_DuplicateSocket(SvcObj* self, i32 fd, u64 reserved, i32* ret, u32* bsd_errno) {
    Bsd* b = self->data;
    if (!b->system) return reply(-SNX_EINVAL, ret, bsd_errno);
    WITH_SOCKET(fd);
    return reply(fd_add(b, s, d.flags, d.exempt), ret, bsd_errno);
}
static int nn_socket_sf_IClient_EventFd(SvcObj* self, u64 initval, u32 flags, i32* ret, u32* bsd_errno) {
    SnxSocket* s;
    int r = snx_socket_eventfd(&s, initval, flags & BSD_EFD_SEMAPHORE);
    if (r) return reply(r, ret, bsd_errno);
    if (flags & BSD_EFD_NONBLOCK) snx_socket_set_nonblocking(s, 1);
    return reply(fd_add(self->data, s, (flags & BSD_EFD_NONBLOCK) ? BSD_O_NONBLOCK_NX : 0, 0), ret, bsd_errno);
}
static SvcObj* bsd_new(const char* name, int system) {
    SvcObj* o = SVCOBJ_NEW_DATA(name, nn_socket_sf_IClient_dispatch, g_hle.bsd.bsds, g_hle.bsd.bsd_next, "bsd sessions");
    Bsd* b = o->data;
    pm_init(&b->lk);
    b->system = system;
    o->destroy = bsd_destroy;
    return o;
}
SvcObj* new_bsd_u(void) { return bsd_new("bsd:u", 0); }
SvcObj* new_bsd_s(void) { return bsd_new("bsd:s", 1); }
SvcObj* new_bsdcfg(void) { return nn_bsdsocket_cfg_ServerInterface_new(); }
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_bsd_state_fns,
               STATE_FN("bsd", bsd_destroy));
