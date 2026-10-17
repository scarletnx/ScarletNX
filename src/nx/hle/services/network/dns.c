/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/hle/service/sockets/sfdnsres.cpp and nsd.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   sfdnsres, the resolver behind the SDK's getaddrinfo and gethostbyname, on the host's resolver, and nsd, which
   turns the console's placeholder FQDNs into names. The console only resolves IPv4. The serialized hostent and
   addrinfo are libnx's resolver.c layout, which Eden verified against the console: big-endian fields, except the
   address and port, which the console passes through htonl once more. */
#include "nx/hle/services/network/network.h"
#include "hle/services/network/dns.swipc.inc"
#ifndef _WIN32
#include <strings.h>
#endif

enum { NETDB_INTERNAL = -1, NETDB_SUCCESS = 0, NETDB_HOST_NOT_FOUND = 1, NETDB_TRY_AGAIN = 2, NETDB_NO_RECOVERY = 3 };
#define ADDRINFO_MAGIC 0xBEEFCAFEu

static int blocked(const char* host) {
    size_t n = strlen(host);
#define BLOCKED_HOST(d)                                                                                                \
    {                                                                                                                  \
        size_t k = sizeof(d) - 1;                                                                                      \
        if (n >= k && !strcasecmp(host + n - k, d) && (n == k || host[n - k - 1] == '.')) return 1;                    \
    }
#include "nx/hle/services/network/abi/blocked_hosts.inc"
    return 0;
}
/* what the game may resolve: anything online; offline only numbers and localhost, as a console without a
   connection, whose resolver can't reach a server */
static int resolvable(const char* host) {
    uint8_t ip[4];
    if (network_online() || !host[0] || !strcasecmp(host, "localhost") || snx_net_parse_ipv4(host, ip)) return 1;
    return 0;
}
/* Eden's mappings, checked on the console though not exhaustively */
static int gai_netdb(int e) {
    return !e || e == SNX_EAI_SERVICE ? NETDB_SUCCESS : e == SNX_EAI_AGAIN ? NETDB_TRY_AGAIN : NETDB_HOST_NOT_FOUND;
}
static int gai_errno(int e) { return e == SNX_EAI_SERVICE ? SNX_EINVAL : 0; }

/* the buffer's text, NUL-terminated within cap */
static void buf_string(IpcBuf b, char* out, size_t cap) {
    out[0] = 0;
    if (!b.addr || !b.size) return;
    size_t n = b.size < cap - 1 ? (size_t)b.size : cap - 1;
    memcpy(out, GP(b.addr), n);
    out[n] = 0;
}

typedef struct {
    u8* p;
    size_t n, cap;
} Out;
static void put(Out* o, const void* v, size_t n) {
    if (o->n + n <= o->cap) memcpy(o->p + o->n, v, n);
    o->n += n;
}
static void put_be32(Out* o, u32 v) {
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    put(o, b, 4);
}
static void put_be16(Out* o, u16 v) {
    u8 b[2] = {(u8)(v >> 8), (u8)v};
    put(o, b, 2);
}
static void put_reversed(Out* o, const uint8_t* v, size_t n) { /* a big-endian field passed through htonl again */
    for (size_t i = 0; i < n; i++) put(o, &v[n - 1 - i], 1);
}

/* the guest's name lookup, 0 or SNX_EAI_*. IPv4 only */
static int lookup(const char* host, const char* service, int socktype, int protocol, int flags, SnxAddrInfo** out) {
    *out = NULL;
    if (blocked(host)) {
        LOG("sfdnsres: %s is blocked, answering EAI_AGAIN\n", host);
        return SNX_EAI_AGAIN;
    }
    if (!resolvable(host)) return SNX_EAI_AGAIN;
    SnxAddrInfo hints = {flags & (SNX_AI_PASSIVE | SNX_AI_CANONNAME | SNX_AI_NUMERICHOST | SNX_AI_NUMERICSERV),
                         SNX_AF_INET,
                         socktype,
                         protocol,
                         {0},
                         NULL,
                         NULL};
    int r = snx_net_getaddrinfo(host[0] ? host : NULL, service && service[0] ? service : NULL, &hints, out);
    if (r) LOG("sfdnsres: %s: %s\n", host, snx_net_gai_strerror(r));
    return r;
}

/* GetHostByNameRequest(WithOptions): the hostent in its buffer. Returns the size, *gai the error */
static u32 host_by_name(IpcBuf name, IpcBuf hostent, int* gai) {
    char host[256];
    buf_string(name, host, sizeof host);
    SnxAddrInfo* list;
    *gai = lookup(host, NULL, SNX_SOCK_STREAM, 0, 0, &list);
    if (*gai) return 0;
    Out o = {hostent.addr ? GP(hostent.addr) : NULL, 0, hostent.addr ? hostent.size : 0};
    put(&o, host, strlen(host) + 1); /* h_name: the name asked for */
    put_be32(&o, 0);                 /* no aliases */
    put_be16(&o, 2);                 /* h_addrtype AF_INET */
    put_be16(&o, 4);                 /* h_length */
    u32 count = 0;
    for (SnxAddrInfo* e = list; e; e = e->next) count++;
    put_be32(&o, count);
    for (SnxAddrInfo* e = list; e; e = e->next) put_reversed(&o, e->addr.addr, 4);
    snx_net_freeaddrinfo(list);
    LOG("sfdnsres: %s resolved (%u addresses)\n", host, count);
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
/* GetAddrInfoRequest(WithOptions): the host, the service and the serialized hints; the list in the response */
static u32 addr_info(IpcBuf host_buf, IpcBuf service_buf, IpcBuf hints, IpcBuf response, int* gai) {
    char host[256], service[64];
    buf_string(host_buf, host, sizeof host);
    buf_string(service_buf, service, sizeof service);
    int flags = 0, socktype = 0, protocol = 0;
    if (hints.addr && hints.size >= 24) {
        const u8* h = GP(hints.addr);
        u32 f[6];
        for (int i = 0; i < 6; i++)
            f[i] = (u32)h[i * 4] << 24 | (u32)h[i * 4 + 1] << 16 | (u32)h[i * 4 + 2] << 8 | h[i * 4 + 3];
        if (f[0] == ADDRINFO_MAGIC) {
            flags = (int)f[1]; /* FreeBSD's AI_ flags, the stack's too */
            socktype = (int)f[3];
            protocol = (int)f[4];
        }
    }
    SnxAddrInfo* list;
    *gai = lookup(host, service, socktype, protocol, flags, &list);
    if (*gai) return 0;
    Out o = {response.addr ? GP(response.addr) : NULL, 0, response.addr ? response.size : 0};
    for (SnxAddrInfo* e = list; e; e = e->next) {
        put_be32(&o, ADDRINFO_MAGIC);
        put_be32(&o, 0); /* ai_flags */
        put_be32(&o, 2); /* AF_INET */
        put_be32(&o, (u32)e->socktype);
        put_be32(&o, (u32)e->protocol);
        put_be32(&o, 16); /* ai_addrlen */
        put_be16(&o, 2);  /* sin_family */
        put_reversed(&o, (const uint8_t*)&e->addr.port, 2);
        put_reversed(&o, e->addr.addr, 4);
        u8 zero[8] = {0};
        put(&o, zero, 8);
        if (e->canonname)
            put(&o, e->canonname, strlen(e->canonname) + 1);
        else
            put(&o, zero, 1);
    }
    u8 sentinel[4] = {0};
    put(&o, sentinel, 4);
    snx_net_freeaddrinfo(list);
    LOG("sfdnsres: %s resolved\n", host);
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
/* GetHostByAddrRequest: {in_addr} in, the hostent with the name out */
static u32 host_by_addr(IpcBuf addr, IpcBuf hostent, int* netdb) {
    *netdb = NETDB_HOST_NOT_FOUND;
    if (!addr.addr || addr.size < 4) return 0;
    SnxSockAddr sa = {SNX_AF_INET, 0, 0, {0}, 0};
    memcpy(sa.addr, GP(addr.addr), 4);
    char name[256];
    if (!network_reachable(&sa) || snx_net_getnameinfo(&sa, name, sizeof name, NULL, 0, SNX_NI_NAMEREQD)) return 0;
    Out o = {hostent.addr ? GP(hostent.addr) : NULL, 0, hostent.addr ? hostent.size : 0};
    put(&o, name, strlen(name) + 1);
    put_be32(&o, 0);
    put_be16(&o, 2);
    put_be16(&o, 4);
    put_be32(&o, 1);
    put_reversed(&o, sa.addr, 4);
    *netdb = NETDB_SUCCESS;
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
static int write_text(IpcBuf b, const char* t) {
    if (!b.addr || !b.size) return 0;
    size_t n = strlen(t) + 1 < b.size ? strlen(t) + 1 : (size_t)b.size;
    memcpy(GP(b.addr), t, n);
    ((char*)GP(b.addr))[n - 1] = 0;
    return 0;
}
static const char* netdb_text(int e) {
    switch (e) {
        case NETDB_SUCCESS: return "Resolver Error 0 (no error)";
        case NETDB_HOST_NOT_FOUND: return "Unknown host";
        case NETDB_TRY_AGAIN: return "Host name lookup failure";
        case NETDB_NO_RECOVERY: return "Unknown server error";
        case 4: return "No address associated with name";
    }
    return "Unknown resolver error";
}

/* a request and its WithOptions twin answer the same values in another order (GetAddrInfoRequestWithOptions adds the
   netdb error) */
static int host_by_name_reply(IpcBuf name, IpcBuf hostent, u32* size, u32* netdb_error, u32* bsd_errno) {
    int gai = 0;
    *size = host_by_name(name, hostent, &gai);
    *netdb_error = (u32)gai_netdb(gai);
    *bsd_errno = (u32)gai_errno(gai);
    return 0;
}
static int host_by_addr_reply(IpcBuf addr, IpcBuf hostent, u32* size, u32* netdb_error) {
    int netdb;
    *size = host_by_addr(addr, hostent, &netdb);
    *netdb_error = (u32)netdb;
    return 0;
}
static int addr_info_reply(IpcBuf host, IpcBuf service, IpcBuf hints, IpcBuf response, u32* size, i32* gai_error,
                           u32* bsd_errno) {
    int gai = 0;
    *size = addr_info(host, service, hints, response, &gai);
    *gai_error = gai;
    *bsd_errno = (u32)gai_errno(gai);
    return 0;
}
static int nn_socket_resolver_IResolver_GetHostByNameRequest(SvcObj* self, u8 use_nsd_resolve, u32 cancel_handle,
                                                             u64 pid, IpcBuf name, IpcBuf hostent, u32* netdb_error,
                                                             u32* bsd_errno, u32* size) {
    return host_by_name_reply(name, hostent, size, netdb_error, bsd_errno);
}
static int nn_socket_resolver_IResolver_GetHostByNameRequestWithOptions(SvcObj* self, u32 use_nsd_resolve,
                                                                        u32 cancel_handle, u64 pid, IpcBuf name,
                                                                        IpcBuf hostent, IpcBuf options, u32* size,
                                                                        u32* netdb_error, u32* bsd_errno) {
    return host_by_name_reply(name, hostent, size, netdb_error, bsd_errno);
}
static int nn_socket_resolver_IResolver_GetHostByAddrRequest(SvcObj* self, u32 addr_len, u32 type, u32 cancel_handle,
                                                             u64 pid, IpcBuf addr, IpcBuf hostent, u32* netdb_error,
                                                             u32* bsd_errno, u32* size) {
    return host_by_addr_reply(addr, hostent, size, netdb_error);
}
static int nn_socket_resolver_IResolver_GetHostByAddrRequestWithOptions(SvcObj* self, u32 addr_len, u32 type,
                                                                        u32 unknown, u32 cancel_handle, u64 pid,
                                                                        IpcBuf addr, IpcBuf hostent, IpcBuf options,
                                                                        u32* size, u32* netdb_error, u32* bsd_errno) {
    return host_by_addr_reply(addr, hostent, size, netdb_error);
}
static int nn_socket_resolver_IResolver_GetHostStringErrorRequest(SvcObj* self, i32 error, IpcBuf text) {
    return write_text(text, netdb_text(error));
}
static int nn_socket_resolver_IResolver_GetGaiStringErrorRequest(SvcObj* self, i32 error, IpcBuf text) {
    return write_text(text, snx_net_gai_strerror(error));
}
static int nn_socket_resolver_IResolver_GetAddrInfoRequest(SvcObj* self, u8 use_nsd_resolve, u32 cancel_handle, u64 pid,
                                                           IpcBuf host, IpcBuf service, IpcBuf hints, IpcBuf response,
                                                           u32* bsd_errno, i32* gai_error, u32* size) {
    return addr_info_reply(host, service, hints, response, size, gai_error, bsd_errno);
}
static int nn_socket_resolver_IResolver_GetAddrInfoRequestWithOptions(SvcObj* self, u32 use_nsd_resolve,
                                                                      u32 cancel_handle, u64 pid, IpcBuf host,
                                                                      IpcBuf service, IpcBuf hints, IpcBuf options,
                                                                      IpcBuf response, u32* size, i32* gai_error,
                                                                      u32* netdb_error, u32* bsd_errno) {
    addr_info_reply(host, service, hints, response, size, gai_error, bsd_errno);
    *netdb_error = (u32)gai_netdb(*gai_error);
    return 0;
}
/* requests finish before a cancel could come */
static int nn_socket_resolver_IResolver_GetCancelHandleRequest(SvcObj* self, u64 pid, u32* cancel_handle) {
    *cancel_handle = 1;
    return 0;
}
SvcObj* new_sfdnsres(void) { return nn_socket_resolver_IResolver_new(0); }

#define NSD_ENVIRONMENT "lp1" /* the retail network environment, GetEnvironmentIdentifier's */
/* the name with each '%', the console's placeholder (accounts.%.ndas.srv.nintendo.net), replaced by the environment,
   as the console's nsd does (edenprobe: probe-%.edenprobe.invalid comes back probe-lp1.edenprobe.invalid). A name
   too long for the 0x100 bytes is cut there */
static int resolve(IpcBuf fqdn_in, IpcBuf fqdn_out) {
    char name[0x100], fqdn[0x100];
    buf_string(fqdn_in, name, sizeof name);
    size_t n = 0;
    for (const char* p = name; *p && n + 1 < sizeof fqdn; p++) {
        if (*p != '%') {
            fqdn[n++] = *p;
            continue;
        }
        for (const char* e = NSD_ENVIRONMENT; *e && n + 1 < sizeof fqdn; e++) fqdn[n++] = *e;
    }
    fqdn[n] = 0;
    if (fqdn_out.addr) {
        u64 s = fqdn_out.size;
        memset(GP(fqdn_out.addr), 0, s < sizeof fqdn ? s : sizeof fqdn);
        memcpy(GP(fqdn_out.addr), fqdn, strlen(fqdn) + 1 <= s ? strlen(fqdn) + 1 : s);
    }
    return 0;
}
static int nn_nsd_detail_IManager_Resolve(SvcObj* self, IpcBuf fqdn_in, IpcBuf fqdn_out) {
    return resolve(fqdn_in, fqdn_out);
}
static int nn_nsd_detail_IManager_ResolveEx(SvcObj* self, IpcBuf fqdn_in, IpcBuf fqdn_out, u32* inner_result) {
    return resolve(fqdn_in, fqdn_out);
}
static int nn_nsd_detail_IManager_GetEnvironmentIdentifier(SvcObj* self, IpcBuf identifier) {
    if (identifier.addr && identifier.size >= 8) {
        memset(GP(identifier.addr), 0, 8);
        memcpy(GP(identifier.addr), NSD_ENVIRONMENT, sizeof NSD_ENVIRONMENT);
    }
    return 0;
}
static int nn_nsd_detail_IManager_GetApplicationServerEnvironmentType(SvcObj* self, u8* type) {
    *type = 1; /* Lp (nn::oe's Dd is 0) */
    return 0;
}
/* nsd's server has no pointer buffer to speak of: a libnx client sending its buffers AutoSelect, edenprobe's, gets its
   8 byte environment identifier through B and a 0x100 byte name through A on the console, as the SDK's client sends
   them (Diablo III's nn::nsd proxies: BufferArgumentInfo<0, 22, 8> and <1, 21, 256>), which libnx does only when they
   don't fit the server's pointer buffer */
SvcObj* new_nsd(void) {
    SvcObj* o = nn_nsd_detail_IManager_new(0);
    if (o) o->ptrbuf = 0;
    return o;
}
