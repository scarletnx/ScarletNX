/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Mobile Adapter GB on the link port. libmobile (REONTeam's, LGPL-3.0-or-later, cmake/dependencies.cmake) is the
   adapter: it takes each byte or word the game clocks out in the serial port's Normal mode and gives the one the
   adapter sends back at the next exchange, as an emulator on the clocking side meets it (its mobile_transfer). Its
   callbacks here: its sockets on the emulated network stack (common/network/net.h), never blocking, as libmobile
   asks; its configuration in a file; its timers on the runtime's interval clock (plat_ns). REON's DNS server, when
   given, is the one it asks when a game gives none */
#include "agb/board/mobile.h"
#include "common/network/net.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include <mobile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AgbMobile {
    struct mobile_adapter* adapter;
    SnxSocket* sockets[MOBILE_MAX_CONNECTIONS];
    uint64_t latched[MOBILE_MAX_TIMERS]; /* plat_ns when each timer was latched */
    unsigned char config[MOBILE_CONFIG_SIZE];
    char path[4096];
    uint32_t answer; /* what the adapter sends at the next exchange */
    bool word, trace;
};

static void say(void* user, const char* line) {
    struct AgbMobile* m = user;
    if (m->trace) fprintf(stderr, "agb mobile: %s\n", line);
}
static void serial_disable(void* user) { (void)user; }
static void serial_enable(void* user, bool mode_32bit) {
    struct AgbMobile* m = user;
    m->word = mode_32bit;
    m->answer = mode_32bit ? MOBILE_SERIAL_IDLE_WORD : MOBILE_SERIAL_IDLE_BYTE;
}
static bool config_read(void* user, void* dest, uintptr_t offset, size_t size) {
    memcpy(dest, ((struct AgbMobile*)user)->config + offset, size);
    return true;
}
static bool config_write(void* user, const void* src, uintptr_t offset, size_t size) {
    struct AgbMobile* m = user;
    memcpy(m->config + offset, src, size);
    if (!snx_write_file(m->path, m->config, sizeof m->config)) return true;
    fprintf(stderr, "agb mobile: the adapter's configuration cannot be written to %s\n", m->path);
    return false;
}
static void time_latch(void* user, unsigned timer) { ((struct AgbMobile*)user)->latched[timer] = plat_ns(); }
static bool time_check_ms(void* user, unsigned timer, unsigned ms) {
    return plat_ns() - ((struct AgbMobile*)user)->latched[timer] >= (uint64_t)ms * 1000000u;
}

/* an address of libmobile's as the stack's: IPv4 alone, which every libmobile supports */
static bool address(const struct mobile_addr* from, SnxSockAddr* to) {
    const struct mobile_addr4* a = (const struct mobile_addr4*)from;
    if (!from || from->type != MOBILE_ADDRTYPE_IPV4) return false;
    uint8_t port[2] = {(uint8_t)(a->port >> 8), (uint8_t)a->port};
    memset(to, 0, sizeof *to);
    to->family = SNX_AF_INET;
    memcpy(&to->port, port, 2);
    memcpy(to->addr, a->host, 4);
    return true;
}
static void address_back(const SnxSockAddr* from, struct mobile_addr* to) {
    struct mobile_addr4* a = (struct mobile_addr4*)to;
    uint8_t port[2];
    memcpy(port, &from->port, 2);
    a->type = MOBILE_ADDRTYPE_IPV4;
    a->port = (unsigned)(port[0] << 8 | port[1]);
    memcpy(a->host, from->addr, 4);
}

static bool sock_open(void* user, unsigned conn, enum mobile_socktype type, enum mobile_addrtype addrtype,
                      unsigned bindport) {
    struct AgbMobile* m = user;
    SnxSocket* s = NULL;
    if (addrtype != MOBILE_ADDRTYPE_IPV4 ||
        snx_socket_open(&s, SNX_AF_INET, type == MOBILE_SOCKTYPE_TCP ? SNX_SOCK_STREAM : SNX_SOCK_DGRAM, 0))
        return false;
    snx_socket_set_nonblocking(s, 1);
    int one = 1; /* the guest's byte order, the stack's own options' (net.h) */
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_REUSEADDR, &one, sizeof one);
    SnxSockAddr any = {.family = SNX_AF_INET};
    uint8_t port[2] = {(uint8_t)(bindport >> 8), (uint8_t)bindport};
    memcpy(&any.port, port, 2);
    if (snx_socket_bind(s, &any)) {
        snx_socket_unref(s);
        return false;
    }
    m->sockets[conn] = s;
    return true;
}
static void sock_close(void* user, unsigned conn) {
    struct AgbMobile* m = user;
    if (m->sockets[conn]) snx_socket_unref(m->sockets[conn]);
    m->sockets[conn] = NULL;
}
static int sock_connect(void* user, unsigned conn, const struct mobile_addr* addr) {
    struct AgbMobile* m = user;
    SnxSockAddr to;
    if (!address(addr, &to)) return -1;
    /* asked again until it is made: on its way, then connected (EISCONN), or why it was not */
    int r = snx_socket_connect(m->sockets[conn], &to);
    if (!r || r == -SNX_EISCONN) return 1;
    return r == -SNX_EINPROGRESS || r == -SNX_EALREADY || r == -SNX_EAGAIN ? 0 : -1;
}
static bool sock_listen(void* user, unsigned conn) {
    return !snx_socket_listen(((struct AgbMobile*)user)->sockets[conn], 1);
}
static bool sock_accept(void* user, unsigned conn) {
    struct AgbMobile* m = user;
    SnxSocket* got = NULL;
    if (snx_socket_accept(m->sockets[conn], &got, NULL, 1)) return false;
    snx_socket_unref(m->sockets[conn]); /* the connection takes the listener's place */
    m->sockets[conn] = got;
    return true;
}
static int sock_send(void* user, unsigned conn, const void* data, unsigned size, const struct mobile_addr* addr) {
    struct AgbMobile* m = user;
    SnxSockAddr to;
    int64_t sent = snx_socket_send(m->sockets[conn], data, size, 0, addr && address(addr, &to) ? &to : NULL);
    if (sent == -SNX_EAGAIN) return 0;
    return sent < 0 ? -1 : (int)sent;
}
static int sock_recv(void* user, unsigned conn, void* data, unsigned size, struct mobile_addr* addr) {
    struct AgbMobile* m = user;
    SnxSockAddr from;
    uint8_t probe;
    /* data NULL asks whether the connection still stands: a peek of nothing waiting is alive, 0 bytes is its end */
    int64_t got =
        snx_socket_recv(m->sockets[conn], data ? data : &probe, data ? size : 1, data ? 0 : SNX_MSG_PEEK, &from, NULL);
    if (got == -SNX_EAGAIN) return 0;
    if (got == -SNX_ECONNRESET || (!got && snx_socket_type(m->sockets[conn]) == SNX_SOCK_STREAM)) return -2;
    if (got < 0) return -1;
    if (!data) return 0;
    if (addr && got > 0 && from.family == SNX_AF_INET) address_back(&from, addr);
    return (int)got;
}
static void update_number(void* user, enum mobile_number type, const char* number) {
    struct AgbMobile* m = user;
    if (m->trace)
        fprintf(stderr, "agb mobile: the %s number is %s\n", type == MOBILE_NUMBER_USER ? "adapter's" : "peer's",
                number ? number : "none");
}

struct AgbMobile* agb_mobile_open(const char* config, const uint8_t dns[4], char* why, size_t size) {
    struct AgbMobile* m = calloc(1, sizeof *m);
    if (!m || strlen(config) >= sizeof m->path || snx_net_init() < 0) {
        snprintf(why, size,
                 m ? "the adapter's configuration path or the network stack is not usable (%s)"
                   : "no memory for the adapter (%s)",
                 config);
        free(m);
        return NULL;
    }
    snprintf(m->path, sizeof m->path, "%s", config);
    size_t length = 0;
    unsigned char* kept = snx_read_file(config, &length);
    if (kept && length == sizeof m->config)
        memcpy(m->config, kept, length);
    else if (kept)
        fprintf(stderr, "agb mobile: %s has %zu bytes, not the adapter's %d: the adapter starts unconfigured\n", config,
                length, MOBILE_CONFIG_SIZE);
    free(kept);
    if (!(m->adapter = mobile_new(m))) {
        snprintf(why, size, "no memory for libmobile's adapter");
        free(m);
        return NULL;
    }
    mobile_def_debug_log(m->adapter, say);
    mobile_def_serial_disable(m->adapter, serial_disable);
    mobile_def_serial_enable(m->adapter, serial_enable);
    mobile_def_config_read(m->adapter, config_read);
    mobile_def_config_write(m->adapter, config_write);
    mobile_def_time_latch(m->adapter, time_latch);
    mobile_def_time_check_ms(m->adapter, time_check_ms);
    mobile_def_sock_open(m->adapter, sock_open);
    mobile_def_sock_close(m->adapter, sock_close);
    mobile_def_sock_connect(m->adapter, sock_connect);
    mobile_def_sock_listen(m->adapter, sock_listen);
    mobile_def_sock_accept(m->adapter, sock_accept);
    mobile_def_sock_send(m->adapter, sock_send);
    mobile_def_sock_recv(m->adapter, sock_recv);
    mobile_def_update_number(m->adapter, update_number);
    mobile_config_load(m->adapter);
    /* libmobile keeps its DNS server in the file it saves at its stop: set each start, none when none is given, so the
       file never holds one the setting no longer names */
    struct mobile_addr4 a = {.type = dns ? MOBILE_ADDRTYPE_IPV4 : MOBILE_ADDRTYPE_NONE, .port = MOBILE_DNS_PORT};
    if (dns) memcpy(a.host, dns, 4);
    mobile_config_set_dns(m->adapter, (const struct mobile_addr*)&a, MOBILE_DNS1);
    for (unsigned t = 0; t < MOBILE_MAX_TIMERS; ++t) time_latch(m, t);
    m->answer = MOBILE_SERIAL_IDLE_BYTE;
    mobile_start(m->adapter);
    return m;
}

void agb_mobile_close(struct AgbMobile* m) {
    if (!m) return;
    mobile_stop(m->adapter);
    for (unsigned c = 0; c < MOBILE_MAX_CONNECTIONS; ++c) sock_close(m, c);
    free(m->adapter);
    free(m);
}

uint32_t agb_mobile_exchange(struct AgbMobile* m, uint32_t sent, bool word) {
    uint32_t back = m->answer;
    /* the adapter's own mode picks the call (libmobile's rule), whatever width the game clocked */
    m->answer = m->word ? mobile_transfer_32bit(m->adapter, sent) : mobile_transfer(m->adapter, (uint8_t)sent);
    return word ? back : back & 0xFF;
}

void agb_mobile_loop(struct AgbMobile* m) { mobile_loop(m->adapter); }

void agb_mobile_trace(struct AgbMobile* m, bool on) { m->trace = on; }
