/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project */
/* soc:U, the console's BSD sockets, on the emulated network stack (common/network/net.h). The wire is Azahar's
   (src/core/hle/service/soc/soc_u.cpp, GPL-2.0-or-later) and libctru's: the console's sockaddr (a length byte, the
   family byte, then the port and the address as the network orders them), its pollfd (the descriptor and two words of
   its own event bits, where libctru's POLLOUT sets the write-band bit), its addrinfo (0x130 bytes), its hostent (0x1A88
   bytes) and its errno numbers (libctru's _net_error_code_map). Host sockets never block here: a call that would block
   a socket the game left blocking parks the calling thread, its request kept, and snx_ctr_soc_poll tries it again on
   each of the scheduler's passes, answering into the thread's command buffer once the socket is ready or the call's
   time is out, as a console's thread waits in the socket module while the game's other threads run. Names go through
   snx_ctr_soc_host first: Pretendo's NAT check hosts with ctr_online=pretendo, none of the console maker's domains */
#include "3ds/services/soc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/system.h"
#include "common/network/net.h"
#include "common/utilities/endian.h"
#include "common/online/ctr.h"
#include "common/online/hosts.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_SOCKETS        64u
#define SOC_WAITS          16u
#define SOC_BAD_PROCESS    0xC8A07004u /* Azahar's ResultWrongProcess */
#define SOC_NOT_STARTED    0xD8E07006u /* ResultNotInitialized: a call before InitializeSockets */
#define SOC_BAD_DESCRIPTOR 0xD8E07007u /* ResultInvalidSocketDescriptor */
#define SOC_STARTED        0xC8A0700Bu /* ResultAlreadyInitialized */
#define SOC_ADDRINFO       0x130u
#define SOC_HOSTENT        0x1A88u
#define SOC_HOSTENT_MOST   24u
#define SOC_DONTWAIT       0x4u /* MSG_DONTWAIT; MSG_OOB 1 and MSG_PEEK 2 are the stack's own values */
#define SOC_NONBLOCK       0x4u /* fcntl's O_NONBLOCK */

typedef struct {
    SnxSocket* socket; /* NULL: a free descriptor */
    bool blocking;     /* the game's view: the host socket never blocks */
} SocSocket;
typedef struct {
    uint32_t thread;   /* the parked thread, 0 for a free slot; it holds a reference while parked */
    uint64_t deadline; /* when a poll's time is out, 0 for no time */
    uint32_t in[SNX_CTR_IPC_WORDS];
} SocWait;
struct SnxCtrSoc {
    bool started;
    SocSocket sockets[SOC_SOCKETS]; /* descriptor i + 1 */
    SocWait waits[SOC_WAITS];
    unsigned said; /* the messages said once, a bit each */
};

/* -1 for the console's EAGAIN, a call that would block: soc_poll tries it again */
#define WOULD_BLOCK INT32_MIN

/* the stack's errno (Linux's numbers, net.h) as the console's, negative (libctru's _net_error_code_map, which Azahar's
   error_map follows) */
static int32_t ctr_error(int e) {
    switch (e) {
        case SNX_E2BIG: return -1;
        case SNX_EACCES: return -2;
        case SNX_EADDRINUSE: return -3;
        case SNX_EADDRNOTAVAIL: return -4;
        case SNX_EAFNOSUPPORT: return -5;
        case SNX_EAGAIN: return -6;
        case SNX_EALREADY: return -7;
        case SNX_EBADF: return -8;
        case SNX_EBUSY: return -10;
        case SNX_ECANCELED: return -11;
        case SNX_ECHILD: return -12;
        case SNX_ECONNABORTED: return -13;
        case SNX_ECONNREFUSED: return -14;
        case SNX_ECONNRESET: return -15;
        case SNX_EDESTADDRREQ: return -17;
        case SNX_EDOM: return -18;
        case SNX_EDQUOT: return -19;
        case SNX_EEXIST: return -20;
        case SNX_EFAULT: return -21;
        case SNX_EFBIG: return -22;
        case SNX_EHOSTUNREACH:
        case SNX_EHOSTDOWN: return -23;
        case SNX_EILSEQ: return -25;
        case SNX_EINPROGRESS: return -26;
        case SNX_EINTR: return -27;
        case SNX_EINVAL: return -28;
        case SNX_EIO: return -29;
        case SNX_EISCONN: return -30;
        case SNX_EISDIR: return -31;
        case SNX_ELOOP: return -32;
        case SNX_EMFILE: return -33;
        case SNX_EMSGSIZE: return -35;
        case SNX_ENAMETOOLONG: return -37;
        case SNX_ENETDOWN: return -38;
        case SNX_ENETRESET: return -39;
        case SNX_ENETUNREACH: return -40;
        case SNX_ENFILE: return -41;
        case SNX_ENOBUFS: return -42;
        case SNX_ENODEV: return -44;
        case SNX_ENOENT: return -45;
        case SNX_ENOMEM: return -49;
        case SNX_ENOMSG: return -50;
        case SNX_ENOPROTOOPT: return -51;
        case SNX_ENOSPC: return -52;
        case SNX_ENOSYS: return -55;
        case SNX_ENOTCONN:
        case SNX_ESHUTDOWN: return -56;
        case SNX_ENOTDIR: return -57;
        case SNX_ENOTSOCK: return -59;
        case SNX_ENOTTY: return -61;
        case SNX_ENXIO: return -62;
        case SNX_EOPNOTSUPP: return -63;
        case SNX_EOVERFLOW: return -64;
        case SNX_EPERM: return -65;
        case SNX_EPIPE: return -66;
        case SNX_EPROTONOSUPPORT: return -68;
        case SNX_EPROTOTYPE: return -69;
        case SNX_ERANGE: return -70;
        case SNX_EROFS: return -71;
        case SNX_ESPIPE: return -72;
        case SNX_ESRCH: return -73;
        case SNX_ESTALE: return -74;
        case SNX_ETIMEDOUT: return -76;
        default:
            fprintf(stderr,
                    "3ds soc: the network stack's error %d has no number of the console's: the game hears "
                    "EINVAL (-28)\n",
                    e);
            return -28;
    }
}
/* getaddrinfo's (SNX_EAI_*) as the console's (Azahar's gai_error_map) */
static int32_t ctr_gai_error(int e) {
    switch (e) {
        case SNX_EAI_AGAIN: return -302;
        case SNX_EAI_FAMILY: return -303;
        case SNX_EAI_MEMORY: return -304;
        case SNX_EAI_SOCKTYPE: return -307;
        default: return -305; /* EAI_NONAME, and what the console has no number for */
    }
}

static struct SnxCtrSoc* state(SnxCtrProcess* p) {
    if (!p->soc) p->soc = calloc(1, sizeof *p->soc);
    return p->soc;
}
static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated, uint32_t result) {
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
static void answer(SnxCtrIpc* c, int32_t value) {
    reply(c, 2, 0, 0);
    c->out[2] = (uint32_t)value;
}

bool snx_ctr_online(void) { return g_settings.network && !g_settings.airplane_mode; }

const char* snx_ctr_soc_host(const char* name) {
    if (!strcmp(g_settings.ctr_online_service, "pretendo")) name = snx_online_pretendo_host(name);
    return snx_online_maker_host(name) ? NULL : name;
}

/* ---- the console's structures ---- */

/* a sockaddr of the console's, length bytes of it: an IPv4 one, into a; false for another family or one too short */
static bool addr_in(const uint8_t* bytes, uint32_t length, SnxSockAddr* a) {
    if (length < 8 || bytes[1] != 2) return false;
    memset(a, 0, sizeof *a);
    a->family = SNX_AF_INET;
    memcpy(&a->port, bytes + 2, 2);
    memcpy(a->addr, bytes + 4, 4);
    return true;
}
static void addr_out(const SnxSockAddr* a, uint8_t out[8]) {
    out[0] = 8, out[1] = 2;
    memcpy(out + 2, &a->port, 2);
    memcpy(out + 4, a->addr, 4);
}
/* reached by a game whose console is offline: loopback alone, as nx's bsd does (network_reachable) */
static bool reachable(const SnxSockAddr* a) {
    return snx_ctr_online() || a->family != SNX_AF_INET || a->addr[0] == 127;
}

/* ---- buffers ---- */

/* the bytes of the static buffer whose descriptor is word i of the request, its address after it, at most cap of them:
   their count, or -1 with the transport's error set */
static int64_t static_in(SnxCtrIpc* c, unsigned i, void* out, uint32_t cap) {
    uint32_t d = c->in[i], size = d >> 14;
    if ((d & 0xf) != 2) {
        snx_ctr_ipc_bad(c);
        return -1;
    }
    if (size > cap) size = cap;
    if (size && snx_ctr_read_ram(c->process->space, c->in[i + 1], out, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return -1;
    }
    return size;
}
/* size bytes into the static buffer id the thread whose TLS is tls set out to receive, and its descriptor at word of
   the reply: at most the buffer's own size. false with the transport's error set */
static bool static_out(SnxCtrIpc* c, uint32_t tls, unsigned id, const void* data, uint32_t size, unsigned word) {
    uint8_t d[8];
    if (snx_ctr_read_ram(c->process->space, tls + 0x180 + id * 8, d, 8) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    uint32_t desc = snx_ld_le32(d), address = snx_ld_le32(d + 4), room = desc >> 14;
    if ((desc & 0xf) != 2) {
        snx_ctr_ipc_bad(c);
        return false;
    }
    if (size > room) size = room;
    if (size && snx_ctr_write_ram(c->process->space, address, data, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    c->out[word] = size << 14 | id << 10 | 2;
    c->out[word + 1] = address;
    return true;
}
/* a mapped buffer's descriptor at word i: its size and address, false when it is not one */
static bool mapped(SnxCtrIpc* c, unsigned i, uint32_t* size, uint32_t* address) {
    if ((c->in[i] & 0xf) != 0xa && (c->in[i] & 0xf) != 0xc && (c->in[i] & 0xf) != 0xe) {
        snx_ctr_ipc_bad(c);
        return false;
    }
    *size = c->in[i] >> 4;
    *address = c->in[i + 1];
    return true;
}

/* ---- descriptors ---- */

static SocSocket* lookup(SnxCtrIpc* c, uint32_t fd) {
    struct SnxCtrSoc* s = state(c->process);
    if (!s || !s->started) {
        reply(c, 1, 0, SOC_NOT_STARTED);
        return NULL;
    }
    if (!fd || fd > SOC_SOCKETS || !s->sockets[fd - 1].socket) {
        reply(c, 1, 0, SOC_BAD_DESCRIPTOR);
        return NULL;
    }
    return &s->sockets[fd - 1];
}
/* a descriptor for socket: its number, or 0 with none left */
static uint32_t descriptor(struct SnxCtrSoc* s, SnxSocket* socket) {
    for (uint32_t i = 0; i < SOC_SOCKETS; ++i)
        if (!s->sockets[i].socket) {
            s->sockets[i].socket = socket;
            s->sockets[i].blocking = true;
            return i + 1;
        }
    return 0;
}

/* ---- the commands; each answers c, or returns WOULD_BLOCK with c unanswered. tls is the asking thread's ---- */

static int32_t socket_open(SnxCtrIpc* c) {
    struct SnxCtrSoc* s = state(c->process);
    uint32_t domain = c->in[1], type = c->in[2], protocol = c->in[3];
    if (!s || !s->started) {
        reply(c, 1, 0, SOC_NOT_STARTED);
        return 0;
    }
    if (domain != 2) return answer(c, -5), 0;             /* EAFNOSUPPORT: IPv4 alone, as Azahar's */
    if (type != 1 && type != 2) return answer(c, -69), 0; /* EPROTOTYPE */
    if (protocol != 0) return answer(c, -68), 0;          /* EPROTONOSUPPORT: the console takes 0 alone */
    SnxSocket* socket = NULL;
    int r = snx_socket_open(&socket, SNX_AF_INET, type == 1 ? SNX_SOCK_STREAM : SNX_SOCK_DGRAM, 0);
    if (r) return answer(c, ctr_error(-r)), 0;
    snx_socket_set_nonblocking(socket, 1);
    uint32_t fd = descriptor(s, socket);
    if (!fd) {
        snx_socket_unref(socket);
        return answer(c, -33), 0; /* EMFILE */
    }
    answer(c, (int32_t)fd);
    return 0;
}

static int32_t socket_accept(SnxCtrIpc* c, uint32_t tls) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return 0;
    SnxSocket* got = NULL;
    SnxSockAddr peer;
    uint8_t addr[8] = {0};
    int r = snx_socket_accept(own->socket, &got, &peer, 1);
    if (r == -SNX_EAGAIN && own->blocking) return WOULD_BLOCK;
    int32_t value;
    if (r)
        value = ctr_error(-r);
    else {
        uint32_t fd = descriptor(state(c->process), got);
        if (!fd) snx_socket_unref(got);
        value = fd ? (int32_t)fd : -33;
        if (fd) addr_out(&peer, addr);
    }
    reply(c, 2, 2, 0);
    c->out[2] = (uint32_t)value;
    static_out(c, tls, 0, addr, value > 0 && c->in[2] >= 8 ? 8 : 0, 3);
    return 0;
}

static int32_t socket_connect(SnxCtrIpc* c, bool retry) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return 0;
    uint8_t bytes[0x1c];
    int64_t got = static_in(c, 5, bytes, sizeof bytes);
    if (got < 0) return 0;
    SnxSockAddr a;
    if (!addr_in(bytes, (uint32_t)got, &a)) return answer(c, -5), 0;
    if (!reachable(&a)) return answer(c, -40), 0; /* ENETUNREACH: offline */
    int r = snx_socket_connect(own->socket, &a);
    /* a connect of a blocking socket waits for its end: asked again, the stack says it is still on its way, then 0 or
       EISCONN once it is made, or why it was not */
    if ((r == -SNX_EINPROGRESS || r == -SNX_EALREADY || r == -SNX_EAGAIN) && own->blocking) return WOULD_BLOCK;
    if (r == -SNX_EISCONN && retry) r = 0; /* the connection the parked call waited for */
    answer(c, r ? ctr_error(-r) : 0);
    return 0;
}

static int32_t socket_bind(SnxCtrIpc* c) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return 0;
    uint8_t bytes[0x1c];
    int64_t got = static_in(c, 5, bytes, sizeof bytes);
    if (got < 0) return 0;
    SnxSockAddr a;
    if (!addr_in(bytes, (uint32_t)got, &a)) return answer(c, -5), 0;
    int r = snx_socket_bind(own->socket, &a);
    answer(c, r ? ctr_error(-r) : 0);
    return 0;
}

/* RecvFrom (0x8: the data in static buffer 0) and RecvFromOther (0x7: in the mapped buffer); the source address in
   static buffer 1 or 0 */
static int32_t socket_recv(SnxCtrIpc* c, uint32_t tls, bool other) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return 0;
    uint32_t length = c->in[2], flags = c->in[3], addr_length = c->in[4], size = length, address = 0;
    if (other && !mapped(c, 7, &size, &address)) return 0;
    if (size > length) size = length;
    if (size > 0x100000u) size = 0x100000u;
    uint8_t* data = malloc(size ? size : 1);
    if (!data) return answer(c, -49), 0; /* ENOMEM */
    SnxSockAddr from = {0};
    int64_t got = snx_socket_recv(own->socket, data, size, (int)(flags & 3), addr_length ? &from : NULL, NULL);
    if (got == -SNX_EAGAIN && own->blocking && !(flags & SOC_DONTWAIT)) {
        free(data);
        return WOULD_BLOCK;
    }
    int32_t value = got < 0 ? ctr_error((int)-got) : (int32_t)got;
    uint8_t addr[8] = {0};
    if (got >= 0 && addr_length && from.family == SNX_AF_INET) addr_out(&from, addr);
    uint32_t addr_size =
        got >= 0 && addr_length && from.family == SNX_AF_INET ? (addr_length < 8 ? addr_length : 8) : 0;
    if (other) {
        if (got > 0 && snx_ctr_write_ram(c->process->space, address, data, (size_t)got) != SNX_CTR_MEM_OK)
            c->transport_result = 0xe0e01bf5u;
        reply(c, 2, 4, 0);
        c->out[2] = (uint32_t)value;
        if (static_out(c, tls, 0, addr, addr_size, 3)) {
            c->out[5] = c->in[7];
            c->out[6] = c->in[8];
        }
    } else {
        reply(c, 3, 4, 0);
        c->out[2] = (uint32_t)value;
        c->out[3] = got > 0 ? (uint32_t)got : 0;
        if (static_out(c, tls, 0, data, got > 0 ? (uint32_t)got : 0, 4)) static_out(c, tls, 1, addr, addr_size, 6);
    }
    free(data);
    return 0;
}

/* SendTo (0xA: the data in static buffer 2, the address in 1) and SendToOther (0x9: the address in static buffer 1,
   the data in the mapped buffer) */
static int32_t socket_send(SnxCtrIpc* c, bool other) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return 0;
    uint32_t length = c->in[2], flags = c->in[3], addr_length = c->in[4], size = length, address = 0;
    unsigned addr_word = other ? 7 : 9;
    if (other && !mapped(c, 9, &size, &address)) return 0;
    if (!other) size = c->in[7] >> 14, address = c->in[8];
    if (size > length) size = length;
    if (size > 0x100000u) size = 0x100000u;
    uint8_t *data = malloc(size ? size : 1), bytes[0x1c];
    if (!data) return answer(c, -49), 0;
    if (size && snx_ctr_read_ram(c->process->space, address, data, size) != SNX_CTR_MEM_OK) {
        free(data);
        c->transport_result = 0xe0e01bf5u;
        return 0;
    }
    SnxSockAddr to;
    const SnxSockAddr* target = NULL;
    int32_t value = 0;
    if (addr_length) {
        int64_t got = static_in(c, addr_word, bytes, sizeof bytes);
        if (got < 0) {
            free(data);
            return 0;
        }
        if (!addr_in(bytes, (uint32_t)got, &to))
            value = -5;
        else if (!reachable(&to))
            value = -40;
        target = &to;
    }
    if (!value) {
        int64_t sent = snx_socket_send(own->socket, data, size, (int)(flags & 3), target);
        if (sent == -SNX_EAGAIN && own->blocking && !(flags & SOC_DONTWAIT)) {
            free(data);
            return WOULD_BLOCK;
        }
        value = sent < 0 ? ctr_error((int)-sent) : (int32_t)sent;
    }
    free(data);
    if (other) {
        reply(c, 2, 2, 0);
        c->out[2] = (uint32_t)value;
        c->out[3] = c->in[9];
        c->out[4] = c->in[10];
    } else
        answer(c, value);
    return 0;
}

static void socket_close(SnxCtrIpc* c) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    snx_socket_unref(own->socket);
    own->socket = NULL;
    answer(c, 0);
}

/* the console's socket options (Azahar's sockopt_map): its level and name as the stack's, false for one it has not */
static bool option(uint32_t level, uint32_t name, int* out_level, int* out_name) {
    static const struct {
        uint32_t level, name;
        int to_level, to_name;
    } map[] = {
        {0xffff, 0x0004, SNX_SOL_SOCKET, SNX_SO_REUSEADDR},
        {0xffff, 0x0080, SNX_SOL_SOCKET, SNX_SO_LINGER},
        {0xffff, 0x0100, SNX_SOL_SOCKET, SNX_SO_OOBINLINE},
        {0xffff, 0x1001, SNX_SOL_SOCKET, SNX_SO_SNDBUF},
        {0xffff, 0x1002, SNX_SOL_SOCKET, SNX_SO_RCVBUF},
        {0xffff, 0x1003, SNX_SOL_SOCKET, SNX_SO_SNDLOWAT},
        {0xffff, 0x1004, SNX_SOL_SOCKET, SNX_SO_RCVLOWAT},
        {0xffff, 0x1008, SNX_SOL_SOCKET, SNX_SO_TYPE},
        {0xffff, 0x1009, SNX_SOL_SOCKET, SNX_SO_ERROR},
        {0, 7, SNX_SOL_IP, SNX_IP_TOS},
        {0, 8, SNX_SOL_IP, SNX_IP_TTL},
        {0, 9, SNX_SOL_IP, SNX_IP_MULTICAST_LOOP},
        {0, 10, SNX_SOL_IP, SNX_IP_MULTICAST_TTL},
        {0, 11, SNX_SOL_IP, SNX_IP_ADD_MEMBERSHIP},
        {0, 12, SNX_SOL_IP, SNX_IP_DROP_MEMBERSHIP},
        {6, 0x2001, SNX_SOL_TCP, SNX_TCP_NODELAY},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i)
        if (map[i].level == level && map[i].name == name) {
            *out_level = map[i].to_level;
            *out_name = map[i].to_name;
            return true;
        }
    return false;
}

static void socket_getsockopt(SnxCtrIpc* c, uint32_t tls) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    int level, name;
    uint8_t value[16] = {0};
    size_t size = c->in[4] < sizeof value ? c->in[4] : sizeof value;
    int r = option(c->in[2], c->in[3], &level, &name) ? snx_socket_getsockopt(own->socket, level, name, value, &size)
                                                      : -SNX_ENOPROTOOPT;
    if (!r && level == SNX_SOL_SOCKET && name == SNX_SO_ERROR && size >= 4) { /* the pending error, the console's */
        int e = (int)snx_ld_le32(value);
        snx_st_le32(value, (uint32_t)(e ? -ctr_error(e) : 0));
    }
    reply(c, 3, 2, 0);
    c->out[2] = (uint32_t)(r ? ctr_error(-r) : 0);
    c->out[3] = r ? 0 : (uint32_t)size;
    static_out(c, tls, 0, value, r ? 0 : (uint32_t)size, 4);
}

static void socket_setsockopt(SnxCtrIpc* c) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    int level, name;
    uint8_t value[16] = {0};
    int64_t got = static_in(c, 7, value, sizeof value);
    if (got < 0) return;
    int r = option(c->in[2], c->in[3], &level, &name)
                ? snx_socket_setsockopt(own->socket, level, name, value, (size_t)got)
                : -SNX_ENOPROTOOPT;
    answer(c, r ? ctr_error(-r) : 0);
}

static void socket_fcntl(SnxCtrIpc* c) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    if (c->in[2] == 3) /* F_GETFL */
        answer(c, own->blocking ? 0 : SOC_NONBLOCK);
    else if (c->in[2] == 4) { /* F_SETFL */
        own->blocking = !(c->in[3] & SOC_NONBLOCK);
        answer(c, 0);
    } else
        answer(c, -28); /* EINVAL: F_GETFL and F_SETFL alone, as Azahar's */
}

static void socket_shutdown(SnxCtrIpc* c) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    int r = c->in[2] > 2 ? -SNX_EINVAL : snx_socket_shutdown(own->socket, (int)c->in[2]);
    answer(c, r ? ctr_error(-r) : 0);
}

static void socket_name(SnxCtrIpc* c, uint32_t tls, bool peer) {
    SocSocket* own = lookup(c, c->in[1]);
    if (!own) return;
    SnxSockAddr a;
    uint8_t addr[8] = {0};
    int r = peer ? snx_socket_peer(own->socket, &a) : snx_socket_name(own->socket, &a);
    if (!r && a.family != SNX_AF_INET) r = -SNX_EAFNOSUPPORT;
    if (!r) addr_out(&a, addr);
    reply(c, 2, 2, 0);
    c->out[2] = (uint32_t)(r ? ctr_error(-r) : 0);
    static_out(c, tls, 0, addr, r ? 0 : (c->in[2] < 8 ? c->in[2] : 8), 3);
}

/* the console's poll bits (Azahar's CTRPollFD::Events): read normal 0x01, read band 0x02 (the console's POLLPRI), 0x04
   priority, write normal 0x08, write band 0x10 (libctru's POLLOUT, taken as POLLOUT), error 0x20, hang-up 0x40, a bad
   descriptor 0x80 */
static int32_t socket_poll(SnxCtrIpc* c, uint32_t tls, bool final) {
    struct SnxCtrSoc* s = state(c->process);
    if (!s || !s->started) {
        reply(c, 1, 0, SOC_NOT_STARTED);
        return 0;
    }
    uint32_t count = c->in[1];
    int32_t timeout = (int32_t)c->in[2];
    if (count > 64) count = 64;
    uint8_t fds[64 * 12];
    int64_t got = static_in(c, 5, fds, count * 12);
    if (got < 0) return 0;
    count = (uint32_t)got / 12;
    SnxPollEntry entries[64];
    bool libctru_out[64];
    int32_t ready = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t fd = snx_ld_le32(fds + i * 12), events = snx_ld_le32(fds + i * 12 + 4);
        entries[i] = (SnxPollEntry){.socket = NULL, .host_fd = -1};
        libctru_out[i] = (events & 0x10) && !(events & 0x08);
        if (fd == UINT32_MAX) continue; /* -1: left out, as the console's poll does */
        if (!fd || fd > SOC_SOCKETS || !s->sockets[fd - 1].socket) {
            snx_st_le32(fds + i * 12 + 8, 0x80); /* POLLNVAL */
            ++ready;
            continue;
        }
        entries[i].socket = s->sockets[fd - 1].socket;
        entries[i].events = (uint16_t)((events & 0x01 ? SNX_POLLIN : 0) | (events & 0x06 ? SNX_POLLPRI : 0) |
                                       (events & 0x18 ? SNX_POLLOUT : 0));
    }
    int r = snx_net_poll(entries, count, 0);
    if (r < 0) {
        reply(c, 2, 2, 0);
        c->out[2] = (uint32_t)ctr_error(-r);
        static_out(c, tls, 0, fds, count * 12, 3);
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!entries[i].socket) continue;
        uint16_t e = entries[i].revents;
        uint32_t out = (e & SNX_POLLIN ? 0x01u : 0) | (e & SNX_POLLPRI ? 0x04u : 0) |
                       (e & SNX_POLLOUT ? (libctru_out[i] ? 0x10u : 0x08u) : 0) | (e & SNX_POLLERR ? 0x20u : 0) |
                       (e & SNX_POLLHUP ? 0x40u : 0) | (e & SNX_POLLNVAL ? 0x80u : 0);
        snx_st_le32(fds + i * 12 + 8, out);
        if (out) ++ready;
    }
    if (!ready && timeout != 0 && !final) return WOULD_BLOCK; /* until the parked request's deadline */
    reply(c, 2, 2, 0);
    c->out[2] = (uint32_t)ready;
    static_out(c, tls, 0, fds, count * 12, 3);
    return 0;
}

/* the addresses of a name: n of them, at most most, the first's canonical name in canon (size bytes). 0, or the
   console's getaddrinfo error */
static int32_t resolve(const char* name, const char* service, const SnxAddrInfo* hints, SnxAddrInfo** list) {
    const char* host = name ? snx_ctr_soc_host(name) : NULL;
    if (name && !host) {
        fprintf(stderr, "3ds soc: %s is the console maker's: it resolves to nothing (EAI_AGAIN)\n", name);
        return -302; /* EAI_AGAIN, as a console that cannot reach it hears */
    }
    SnxAddrInfo h = hints ? *hints : (SnxAddrInfo){0};
    if (!snx_ctr_online()) h.flags |= SNX_AI_NUMERICHOST; /* offline: addresses written as numbers alone */
    int r = snx_net_getaddrinfo(host, service, &h, list);
    return r ? ctr_gai_error(r) : 0;
}

/* GetHostByName (0xD): the name in static buffer 0 (with its NUL), the console's hostent out in static buffer 0 */
static void host_by_name(SnxCtrIpc* c, uint32_t tls) {
    char name[256] = {0};
    if (static_in(c, 3, name, sizeof name - 1) < 0) return;
    SnxAddrInfo hints = {.family = SNX_AF_INET, .socktype = SNX_SOCK_STREAM, .flags = SNX_AI_CANONNAME}, *list = NULL;
    int32_t r = resolve(name, NULL, &hints, &list);
    uint8_t* out = r ? NULL : calloc(1, SOC_HOSTENT);
    if (!r && !out) r = -304; /* EAI_MEMORY */
    uint32_t count = 0;
    if (!r) {
        snx_st_le16(out, 2); /* AF_INET */
        snx_st_le16(out + 2, 4);
        snprintf((char*)out + 8, 256, "%s", list && list->canonname ? list->canonname : name);
        for (const SnxAddrInfo* a = list; a && count < SOC_HOSTENT_MOST; a = a->next)
            if (a->addr.family == SNX_AF_INET)
                memcpy(out + 8 + 256 + SOC_HOSTENT_MOST * 256 + 16 * count++, a->addr.addr, 4);
        snx_st_le16(out + 4, (uint16_t)count);
    }
    snx_net_freeaddrinfo(list);
    reply(c, 2, 2, 0);
    c->out[2] = (uint32_t)r;
    static_out(c, tls, 0, out, r ? 0 : SOC_HOSTENT, 3);
    free(out);
}

/* GetAddrInfo (0xF): the node, the service and the hints in static buffers, their lengths in words 1 to 3, the room
   for the answers (0x130 bytes each) in word 4; the count found, whatever fits, as 3dbrew says the console's does */
static void addr_info(SnxCtrIpc* c, uint32_t tls) {
    char node[256] = {0}, service[64] = {0};
    uint8_t hint[SOC_ADDRINFO] = {0};
    if ((c->in[1] && static_in(c, 5, node, sizeof node - 1) < 0) ||
        (c->in[2] && static_in(c, 7, service, sizeof service - 1) < 0) ||
        (c->in[3] && static_in(c, 9, hint, sizeof hint) < 0))
        return;
    SnxAddrInfo hints = {0}, *list = NULL;
    if (c->in[3]) {
        uint32_t flags = snx_ld_le32(hint), family = snx_ld_le32(hint + 4), type = snx_ld_le32(hint + 8);
        uint32_t protocol = snx_ld_le32(hint + 12);
        hints.flags = (flags & 1 ? SNX_AI_PASSIVE : 0) | (flags & 2 ? SNX_AI_CANONNAME : 0) |
                      (flags & 4 ? SNX_AI_NUMERICHOST : 0) | (flags & 8 ? SNX_AI_NUMERICSERV : 0);
        hints.family = family == 2 ? SNX_AF_INET : SNX_AF_UNSPEC;
        hints.socktype = type == 1 ? SNX_SOCK_STREAM : type == 2 ? SNX_SOCK_DGRAM : 0;
        hints.protocol = (int)protocol;
        /* as the console's soc fills one from the other (Azahar's GetAddrInfoImpl) */
        if (hints.socktype && !hints.protocol) hints.protocol = hints.socktype == SNX_SOCK_STREAM ? 6 : 17;
        if (hints.protocol && !hints.socktype) hints.socktype = hints.protocol == 6 ? SNX_SOCK_STREAM : SNX_SOCK_DGRAM;
    }
    int32_t r = resolve(c->in[1] ? node : NULL, c->in[2] ? service : NULL, c->in[3] ? &hints : NULL, &list);
    uint32_t room = c->in[4] / SOC_ADDRINFO, count = 0;
    uint8_t* out = r || !room ? NULL : calloc(room, SOC_ADDRINFO);
    for (const SnxAddrInfo* a = r ? NULL : list; a; a = a->next) {
        if (a->addr.family != SNX_AF_INET) continue;
        if (out && count < room) {
            uint8_t* e = out + count * SOC_ADDRINFO;
            snx_st_le32(e, (uint32_t)((a->flags & SNX_AI_PASSIVE ? 1 : 0) | (a->flags & SNX_AI_CANONNAME ? 2 : 0)));
            snx_st_le32(e + 4, 2);
            snx_st_le32(e + 8, a->socktype == SNX_SOCK_STREAM ? 1u : 2u);
            snx_st_le32(e + 12, (uint32_t)a->protocol);
            snx_st_le32(e + 16, 8);
            if (a->canonname) snprintf((char*)e + 20, 256, "%s", a->canonname);
            addr_out(&a->addr, e + 20 + 256);
        }
        ++count;
    }
    snx_net_freeaddrinfo(list);
    reply(c, 3, 2, 0);
    c->out[2] = (uint32_t)r;
    c->out[3] = r ? 0 : count;
    static_out(c, tls, 0, out, out ? (count < room ? count : room) * SOC_ADDRINFO : 0, 4);
    free(out);
}

/* GetHostId (0x16): the console's IPv4 address, as the network orders it; 0 offline */
static void host_id(SnxCtrIpc* c) {
    SnxNetInterface iface;
    uint32_t ip = 0;
    if (snx_ctr_online() && !snx_net_primary_interface(&iface)) memcpy(&ip, iface.ip, 4);
    reply(c, 2, 0, 0);
    c->out[2] = ip;
}

static void close_all(struct SnxCtrSoc* s) {
    for (uint32_t i = 0; i < SOC_SOCKETS; ++i)
        if (s->sockets[i].socket) {
            snx_socket_unref(s->sockets[i].socket);
            s->sockets[i].socket = NULL;
        }
}

/* one request: WOULD_BLOCK when it waits on a socket. final: its wait's time is out, it answers whatever it has */
static int32_t run(SnxCtrIpc* c, uint32_t tls, bool retry, bool final) {
    struct SnxCtrSoc* s = state(c->process);
    uint32_t command = c->in[0] >> 16;
    if (!s) {
        reply(c, 1, 0, 0xd86007f3u); /* out of memory */
        return 0;
    }
    switch (command) {
        case 0x0001: /* InitializeSockets: the memory block it gives is not needed */
            if (snx_net_init() < 0) fprintf(stderr, "3ds soc: the network stack does not start\n");
            reply(c, 1, 0, s->started ? SOC_STARTED : 0);
            s->started = true;
            return 0;
        case 0x0002: return socket_open(c);
        case 0x0003: {
            SocSocket* own = lookup(c, c->in[1]);
            if (own) {
                int r = snx_socket_listen(own->socket, (int)c->in[2]);
                answer(c, r ? ctr_error(-r) : 0);
            }
            return 0;
        }
        case 0x0004: return socket_accept(c, tls);
        case 0x0005: return socket_bind(c);
        case 0x0006: return socket_connect(c, retry);
        case 0x0007: return socket_recv(c, tls, true);
        case 0x0008: return socket_recv(c, tls, false);
        case 0x0009: return socket_send(c, true);
        case 0x000A: return socket_send(c, false);
        case 0x000B: socket_close(c); return 0;
        case 0x000C: socket_shutdown(c); return 0;
        case 0x000D: host_by_name(c, tls); return 0;
        case 0x000F: addr_info(c, tls); return 0;
        case 0x0011: socket_getsockopt(c, tls); return 0;
        case 0x0012: socket_setsockopt(c); return 0;
        case 0x0013: socket_fcntl(c); return 0;
        case 0x0014: return socket_poll(c, tls, final);
        case 0x0016: host_id(c); return 0;
        case 0x0017: socket_name(c, tls, false); return 0;
        case 0x0018: socket_name(c, tls, true); return 0;
        case 0x0019: /* ShutdownSockets */
            reply(c, 1, 0, s->started ? 0 : SOC_NOT_STARTED);
            close_all(s);
            s->started = false;
            return 0;
        case 0x0021: /* CloseSockets: the process's */
            close_all(s);
            reply(c, 1, 0, 0);
            return 0;
        default: snx_ctr_system_unmodeled(c, "soc:U", (SNX_CTR_SERVICE_SOC << 16) | command); return 0;
    }
}

void snx_ctr_soc_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (run(c, p->cpu.state.cpu.tpidruro, false, false) != WOULD_BLOCK) return;
    struct SnxCtrSoc* s = p->soc;
    unsigned i = 0;
    while (i < SOC_WAITS && s->waits[i].thread) ++i;
    if (i == SOC_WAITS) { /* more threads wait than the console has: this one hears EAGAIN */
        fprintf(stderr, "3ds soc: %u threads already wait on sockets: command 0x%04x answers EAGAIN\n", SOC_WAITS,
                c->in[0] >> 16);
        reply(c, 2, 0, 0);
        c->out[2] = (uint32_t)-6;
        return;
    }
    SocWait* w = &s->waits[i];
    w->thread = p->current_thread;
    memcpy(w->in, c->in, sizeof w->in);
    int32_t timeout = (c->in[0] >> 16) == 0x0014 ? (int32_t)c->in[2] : -1;
    w->deadline = timeout > 0 ? snx_ctr_now_ns() + (uint64_t)timeout * 1000000ull : 0;
    ++p->objects[w->thread].refs;
    /* parked: the scheduler runs it again once snx_ctr_soc_poll has answered */
    snx_ctr_thread(p)->wake_ns = w->deadline ? w->deadline : UINT64_MAX;
    c->state = SNX_CTR_IPC_BLOCKED;
}

/* the parked request's answer into its thread's command buffer, and the thread woken (uds.c's finish_join) */
static void finish(SnxCtrProcess* p, SocWait* w, const SnxCtrIpc* c) {
    CtrThread* t = &p->objects[w->thread].data.thread;
    uint32_t transport = c->transport_result;
    if (!t->dead && !transport) {
        uint32_t words = 1 + ((c->out[0] >> 6) & 63) + (c->out[0] & 63);
        uint8_t out[SNX_CTR_IPC_WORDS * 4];
        for (uint32_t i = 0; i < words && i < SNX_CTR_IPC_WORDS; ++i) snx_st_le32(out + i * 4, c->out[i]);
        if (snx_ctr_write_ram(p->space, t->context.tpidruro + 0x80, out, words * 4) != SNX_CTR_MEM_OK)
            transport = 0xe0e01bf5u;
    }
    if (!t->dead) {
        t->context.r[0] = transport;
        t->context.exit = t->context.syndrome = 0;
        t->wake_ns = 0;
        if (p->current_thread == w->thread) {
            p->cpu.state.cpu.r[0] = transport;
            p->cpu.state.cpu.exit = p->cpu.state.cpu.syndrome = 0;
        }
        snx_ctr_thread_ready(p, w->thread);
    }
    uint32_t id = w->thread;
    w->thread = 0;
    snx_ctr_object_release(p, id);
}

void snx_ctr_soc_poll(SnxCtrProcess* p) {
    struct SnxCtrSoc* s = p->soc;
    if (!s) return;
    uint64_t now = snx_ctr_now_ns();
    for (unsigned i = 0; i < SOC_WAITS; ++i) {
        SocWait* w = &s->waits[i];
        if (!w->thread) continue;
        CtrThread* t = &p->objects[w->thread].data.thread;
        if (t->dead) {
            uint32_t id = w->thread;
            w->thread = 0;
            snx_ctr_object_release(p, id);
            continue;
        }
        SnxCtrIpc c = {.process = p, .object = 0, .caller_pid = CTR_PROCESS_ID};
        memcpy(c.in, w->in, sizeof c.in);
        bool final = w->deadline && now >= w->deadline;
        if (run(&c, t->context.tpidruro, true, final) == WOULD_BLOCK) continue;
        finish(p, w, &c);
    }
}

unsigned snx_ctr_soc_open(const SnxCtrProcess* p) {
    unsigned n = 0;
    for (unsigned i = 0; p->soc && i < SOC_SOCKETS; ++i) n += p->soc->sockets[i].socket != NULL;
    return n;
}
bool snx_ctr_soc_waiting(const SnxCtrProcess* p) {
    if (!p->soc) return false;
    for (unsigned i = 0; i < SOC_WAITS; ++i)
        if (p->soc->waits[i].thread) return true;
    return false;
}

void snx_ctr_soc_close(SnxCtrProcess* p) {
    struct SnxCtrSoc* s = p->soc;
    if (!s) return;
    for (unsigned i = 0; i < SOC_WAITS; ++i)
        if (s->waits[i].thread) snx_ctr_object_release(p, s->waits[i].thread);
    close_all(s);
    free(s);
    p->soc = NULL;
}
