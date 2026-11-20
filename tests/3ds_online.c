/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's network path, as a game's requests reach it: soc:U's sockets on loopback (a datagram there and back, a
   blocking receive that parks its thread until the datagram comes, poll with libctru's POLLOUT, a TCP connection, a
   socket made non-blocking, the console offline), its names (the console maker's resolve to nothing, Pretendo's NAT
   check hosts rewritten), ac:u's connection online and offline, and frd:u's login to a game's server through a NASC on
   this host that answers as Pretendo's does: the LOGIN form the game's request makes, the reply's server and token in
   GetGameAuthenticationData */
#include "3ds/runtime/internal.h"
#include "3ds/services/frd.h"
#include "3ds/services/soc.h"
#include "common/network/net.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/online/nasc.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA      0x300000u /* the data page, then the BSS: 0x11000 bytes the test lays its buffers in */
#define ADDR      (DATA + 0x100)
#define ADDR2     (DATA + 0x140)
#define BYTES     (DATA + 0x200)
#define RECEIVED  (DATA + 0x1000)
#define RECEIVED2 (DATA + 0x3000)
#define TLS       SNX_CTR_INITIAL_TLS

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "3ds_online: %s failed\n", what);
        exit(1);
    }
}
static void bytes_put(SnxCtrProcess* p, uint32_t va, const void* data, size_t size) {
    check(snx_ctr_write_ram(p->space, va, data, size) == SNX_CTR_MEM_OK, "a write of guest memory");
}
static void bytes_get(SnxCtrProcess* p, uint32_t va, void* data, size_t size) {
    check(snx_ctr_read_ram(p->space, va, data, size) == SNX_CTR_MEM_OK, "a read of guest memory");
}
/* the thread's receive static buffer id: size bytes at va */
static void receive_buffer(SnxCtrProcess* p, unsigned id, uint32_t va, uint32_t size) {
    ctr_put(p, TLS + 0x180 + id * 8, size << 14 | id << 10 | 2);
    ctr_put(p, TLS + 0x184 + id * 8, va);
}
/* a request sent: how the SVC ended, the reply's words then in the command buffer */
static SnxCtrSvcResult send(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count) {
    uint32_t result = 0xffffffffu;
    ctr_request(p, words, count);
    SnxCtrSvcResult state = snx_ctr_ipc_send(p, handle, &result);
    if (state == SNX_CTR_SVC_RETURN) check(!result, "a request's transport");
    return state;
}
/* a request answered at once: its reply's word 2, the call's value (word 1, the result, checked zero) */
static int32_t call(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count) {
    check(send(p, handle, words, count) == SNX_CTR_SVC_RETURN, "a request answered at once");
    check(!ctr_command_word(p, 1), "a request's result");
    return (int32_t)ctr_command_word(p, 2);
}
/* a 3DS sockaddr_in at va: 127.0.0.1 (or ip) and port, as the network orders them */
static void sockaddr_put(SnxCtrProcess* p, uint32_t va, const uint8_t ip[4], uint16_t port) {
    uint8_t a[8] = {8, 2, (uint8_t)(port >> 8), (uint8_t)port, ip[0], ip[1], ip[2], ip[3]};
    bytes_put(p, va, a, sizeof a);
}
static const uint8_t loopback[4] = {127, 0, 0, 1};

static int32_t soc_socket(SnxCtrProcess* p, uint32_t soc, uint32_t type) {
    const uint32_t w[] = {0x000200C2u, 2, type, 0, 0x20, 0};
    return call(p, soc, w, 6);
}
static int32_t soc_bind(SnxCtrProcess* p, uint32_t soc, int32_t fd, uint32_t addr) {
    const uint32_t w[] = {0x00050084u, (uint32_t)fd, 8, 0x20, 0, 8u << 14 | 2, addr};
    return call(p, soc, w, 7);
}
static uint16_t soc_port(SnxCtrProcess* p, uint32_t soc, int32_t fd) {
    const uint32_t w[] = {0x00170082u, (uint32_t)fd, 8, 0x20, 0};
    receive_buffer(p, 0, ADDR2, 8);
    check(call(p, soc, w, 5) == 0, "GetSockName");
    uint8_t a[8];
    bytes_get(p, ADDR2, a, 8);
    check(a[0] == 8 && a[1] == 2 && !memcmp(a + 4, loopback, 4), "GetSockName's address");
    return (uint16_t)(a[2] << 8 | a[3]);
}
static int32_t soc_sendto(SnxCtrProcess* p, uint32_t soc, int32_t fd, const char* text, uint32_t addr) {
    uint32_t n = (uint32_t)strlen(text);
    bytes_put(p, BYTES, text, n);
    const uint32_t w[] = {0x000A0106u, (uint32_t)fd,           n,   0, 8, 0x20, 0, n << 14 | 2 << 10 | 2,
                          BYTES,       8u << 14 | 1 << 10 | 2, addr};
    return call(p, soc, w, 11);
}
static const uint32_t* recvfrom_words(int32_t fd, uint32_t flags) {
    static uint32_t w[7];
    const uint32_t words[] = {0x00080102u, (uint32_t)fd, 256, flags, 8, 0x20, 0};
    memcpy(w, words, sizeof w);
    return w;
}

static void sockets(SnxCtrProcess* p, uint32_t soc) {
    /* before InitializeSockets: the console's not-initialized result */
    const uint32_t early[] = {0x000200C2u, 2, 2, 0, 0x20, 0};
    check(send(p, soc, early, 6) == SNX_CTR_SVC_RETURN && ctr_command_word(p, 1) == 0xD8E07006u,
          "Socket before InitializeSockets");
    const uint32_t init[] = {0x00010044u, 0x4000, 0x20, 0, 0, 0};
    check(send(p, soc, init, 6) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1), "InitializeSockets");
    check(send(p, soc, init, 6) == SNX_CTR_SVC_RETURN && ctr_command_word(p, 1) == 0xC8A0700Bu,
          "InitializeSockets twice");
    /* two datagram sockets on loopback, one sends, the other receives */
    int32_t a = soc_socket(p, soc, 2), b = soc_socket(p, soc, 2);
    check(a > 0 && b > 0 && a != b, "Socket");
    check(soc_socket(p, soc, 3) == -69, "Socket of a type the console has not: EPROTOTYPE");
    sockaddr_put(p, ADDR, loopback, 0);
    check(soc_bind(p, soc, a, ADDR) == 0, "Bind");
    uint16_t port = soc_port(p, soc, a);
    check(port != 0, "a bound port");
    sockaddr_put(p, ADDR, loopback, port);
    check(soc_sendto(p, soc, b, "hello", ADDR) == 5, "SendTo");
    receive_buffer(p, 0, RECEIVED, 256);
    receive_buffer(p, 1, ADDR2, 8);
    int32_t got = 0;
    for (int tries = 0; tries < 200 && got <= 0; ++tries) { /* loopback is quick, not instant */
        const uint32_t* w = recvfrom_words(a, 4);           /* MSG_DONTWAIT */
        got = call(p, soc, w, 7);
        if (got == -6) plat_sleep_ns(5000000);
    }
    char text[16] = {0};
    bytes_get(p, RECEIVED, text, 5);
    check(got == 5 && ctr_command_word(p, 3) == 5 && !strcmp(text, "hello"), "RecvFrom's datagram");
    uint8_t from[8];
    bytes_get(p, ADDR2, from, 8);
    check(from[1] == 2 && !memcmp(from + 4, loopback, 4), "RecvFrom's source");
    /* a blocking receive with nothing to read parks the thread; the datagram's arrival answers it */
    check(send(p, soc, recvfrom_words(a, 0), 7) == SNX_CTR_SVC_BLOCKED, "a blocking RecvFrom parks its thread");
    check(snx_ctr_soc_waiting(p), "the parked receive counted");
    check(soc_sendto(p, soc, b, "late", ADDR) == 4, "SendTo after the park");
    for (int tries = 0; tries < 200 && snx_ctr_soc_waiting(p); ++tries) {
        snx_ctr_soc_poll(p);
        if (snx_ctr_soc_waiting(p)) plat_sleep_ns(5000000);
    }
    memset(text, 0, sizeof text);
    bytes_get(p, RECEIVED, text, 4);
    check(!snx_ctr_soc_waiting(p) && ctr_command_word(p, 2) == 4 && !strcmp(text, "late") && !p->cpu.state.cpu.r[0],
          "the parked receive answered into the thread's command buffer");
    /* poll: nothing to read on a, b writable as libctru asks it (its POLLOUT is the console's write band) */
    uint8_t fds[24] = {0};
    snx_st_le32(fds, (uint32_t)a), snx_st_le32(fds + 4, 0x01);
    snx_st_le32(fds + 12, (uint32_t)b), snx_st_le32(fds + 16, 0x10);
    bytes_put(p, BYTES, fds, sizeof fds);
    receive_buffer(p, 0, RECEIVED, 24);
    const uint32_t poll[] = {0x00140084u, 2, 0, 0x20, 0, 24u << 14 | 10 << 10 | 2, BYTES};
    check(call(p, soc, poll, 7) == 1, "Poll's count");
    bytes_get(p, RECEIVED, fds, sizeof fds);
    check(snx_ld_le32(fds + 8) == 0 && snx_ld_le32(fds + 20) == 0x10, "Poll's events");
    /* F_SETFL O_NONBLOCK: an empty receive says EAGAIN instead of parking */
    const uint32_t nonblock[] = {0x001300C2u, (uint32_t)a, 4, 4, 0x20, 0};
    const uint32_t getfl[] = {0x001300C2u, (uint32_t)a, 3, 0, 0x20, 0};
    check(call(p, soc, nonblock, 6) == 0 && call(p, soc, getfl, 6) == 4, "Fcntl");
    receive_buffer(p, 0, RECEIVED, 256);
    check(call(p, soc, recvfrom_words(a, 0), 7) == -6, "a non-blocking empty receive: EAGAIN");
    /* a TCP connection on loopback */
    int32_t listener = soc_socket(p, soc, 1), client = soc_socket(p, soc, 1);
    sockaddr_put(p, ADDR, loopback, 0);
    check(soc_bind(p, soc, listener, ADDR) == 0, "Bind of the listener");
    const uint32_t listen[] = {0x00030082u, (uint32_t)listener, 4, 0x20, 0};
    check(call(p, soc, listen, 5) == 0, "Listen");
    sockaddr_put(p, ADDR, loopback, soc_port(p, soc, listener));
    const uint32_t connect[] = {0x00060084u, (uint32_t)client, 8, 0x20, 0, 8u << 14 | 2, ADDR};
    SnxCtrSvcResult state = send(p, soc, connect, 7);
    for (int tries = 0; tries < 200 && snx_ctr_soc_waiting(p); ++tries) {
        snx_ctr_soc_poll(p);
        if (snx_ctr_soc_waiting(p)) plat_sleep_ns(5000000);
    }
    check((state == SNX_CTR_SVC_RETURN || state == SNX_CTR_SVC_BLOCKED) && !ctr_command_word(p, 1) &&
              ctr_command_word(p, 2) == 0,
          "Connect");
    receive_buffer(p, 0, ADDR2, 8);
    const uint32_t accept[] = {0x00040082u, (uint32_t)listener, 8, 0x20, 0};
    int32_t served = call(p, soc, accept, 5);
    check(served > 0, "Accept");
    const uint32_t close[] = {0x000B0042u, (uint32_t)served, 0x20, 0};
    check(call(p, soc, close, 4) == 0, "Close");
    check(send(p, soc, close, 4) == SNX_CTR_SVC_RETURN && ctr_command_word(p, 1) == 0xD8E07007u,
          "Close of a closed descriptor");
    /* offline: loopback still, any other address unreachable */
    g_settings.network = 0;
    const uint8_t elsewhere[4] = {192, 0, 2, 1};
    sockaddr_put(p, ADDR, elsewhere, 53);
    check(soc_sendto(p, soc, b, "x", ADDR) == -40, "SendTo off loopback offline: ENETUNREACH");
    sockaddr_put(p, ADDR, loopback, port);
    check(soc_sendto(p, soc, b, "x", ADDR) == 1, "SendTo on loopback offline");
    /* names: the console maker's resolve to nothing; a numeric one resolves offline too */
    receive_buffer(p, 0, RECEIVED2, 0x130);
    const char maker[] = "nasc.nintendowifi.net", number[] = "127.0.0.1";
    bytes_put(p, BYTES, maker, sizeof maker);
    const uint32_t gai_maker[] = {0x000F0106u, sizeof maker, 0, 0,           0x130, sizeof maker << 14 | 5 << 10 | 2,
                                  BYTES,       6 << 10 | 2,  0, 7 << 10 | 2, 0};
    check(call(p, soc, gai_maker, 11) == -302 && ctr_command_word(p, 3) == 0, "GetAddrInfo of the maker's");
    bytes_put(p, BYTES, number, sizeof number);
    const uint32_t gai_number[] = {0x000F0106u, sizeof number, 0, 0,           0x130, sizeof number << 14 | 5 << 10 | 2,
                                   BYTES,       6 << 10 | 2,   0, 7 << 10 | 2, 0};
    check(call(p, soc, gai_number, 11) == 0 && ctr_command_word(p, 3) >= 1, "GetAddrInfo of a number");
    uint8_t info[0x130];
    bytes_get(p, RECEIVED2, info, sizeof info);
    check(snx_ld_le32(info + 4) == 2 && info[20 + 256 + 1] == 2 && !memcmp(info + 20 + 256 + 4, loopback, 4),
          "GetAddrInfo's answer");
    g_settings.network = 1;
    snprintf(g_settings.ctr_online_service, sizeof g_settings.ctr_online_service, "pretendo");
    check(!strcmp(snx_ctr_soc_host("nncs1.app.nintendowifi.net"), "nncs1.app.pretendo.cc") &&
              !snx_ctr_soc_host("account.nintendo.net") && !strcmp(snx_ctr_soc_host("example.org"), "example.org"),
          "the names as Pretendo's Nimbus rewrites them");
    snprintf(g_settings.ctr_online_service, sizeof g_settings.ctr_online_service, "off");
    check(!snx_ctr_soc_host("nncs1.app.nintendowifi.net"), "the NAT check host with online off");
    const uint32_t shut[] = {0x00190000u};
    check(send(p, soc, shut, 1) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1), "ShutdownSockets");
}

static uint32_t event_new(SnxCtrProcess* p) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[1] = 0; /* one-shot */
    check(snx_ctr_svc(p, 0x17) == SNX_CTR_SVC_RETURN && !r[0], "CreateEvent");
    return r[1];
}
static bool signaled(SnxCtrProcess* p, uint32_t handle) {
    return p->objects[snx_ctr_handle_object(p, handle)].data.event.signaled != 0;
}

static void connection(SnxCtrProcess* p, uint32_t ac) {
    uint32_t event = event_new(p);
    const uint32_t connect[] = {0x00040006u, 0x20, 0, 0, event, 0x200u << 14 | 1 << 10 | 2, RECEIVED};
    const uint32_t result[] = {0x00050002u, 0x20, 0}, wifi[] = {0x000D0000u};
    g_settings.network = 0;
    check(send(p, ac, connect, 7) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1) && signaled(p, event),
          "ConnectAsync offline signals its event");
    check(send(p, ac, result, 3) == SNX_CTR_SVC_RETURN && ctr_command_word(p, 1) == 0xE0A09D2Eu,
          "GetConnectResult offline: not connected");
    check(call(p, ac, wifi, 1) == 0, "GetWifiStatus offline");
    g_settings.network = 1;
    p->objects[snx_ctr_handle_object(p, event)].data.event.signaled = 0;
    check(send(p, ac, connect, 7) == SNX_CTR_SVC_RETURN && signaled(p, event), "ConnectAsync online");
    check(send(p, ac, result, 3) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1), "GetConnectResult online");
    check(call(p, ac, wifi, 1) == 1, "GetWifiStatus online: the first slot");
}

/* ---- a NASC on this host, answering one LOGIN as Pretendo's does ---- */
typedef struct {
    SnxSocket* listener;
    _Atomic int done;
    char form[4096];
} Nasc;
static void nasc_serve(void* arg) {
    Nasc* n = arg;
    SnxSocket* s = NULL;
    char request[8192] = {0};
    size_t have = 0;
    if (snx_socket_accept(n->listener, &s, NULL, 0)) goto end;
    for (;;) { /* the headers, then Content-Length's bytes */
        int64_t got = snx_socket_recv(s, request + have, sizeof request - 1 - have, 0, NULL, NULL);
        if (got <= 0) break;
        have += (size_t)got;
        char* body = strstr(request, "\r\n\r\n");
        char* length = strstr(request, "Content-Length: ");
        if (body && length && (size_t)(request + have - (body + 4)) >= (size_t)atol(length + 16)) {
            snprintf(n->form, sizeof n->form, "%s", body + 4);
            break;
        }
    }
    char reply[1024], locator[64], date[32], out[1300];
    snx_online_b64n_encode(locator, sizeof locator, "127.0.0.1:60123", 15);
    snx_online_b64n_encode(date, sizeof date, "20261119013000", 14);
    snprintf(reply, sizeof reply, "locator=%s&retry=MA**&returncd=MDAx&token=VE9LRU4tT0Y-U0NBUkxFVA**&datetime=%s",
             locator, date);
    snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(reply), reply);
    snx_socket_send(s, out, strlen(out), 0, NULL);
    snx_socket_unref(s);
end:
    atomic_store(&n->done, 1);
}
/* the form's field key, decoded: its text */
static const char* field(const char* form, const char* key, char* out, size_t cap) {
    size_t k = strlen(key);
    out[0] = 0;
    for (const char* at = form; at && *at; at = strchr(at, '&') ? strchr(at, '&') + 1 : NULL) {
        if (strncmp(at, key, k) || at[k] != '=') continue;
        const char *value = at + k + 1, *end = strchr(value, '&');
        long got = snx_online_b64n_decode((uint8_t*)out, cap - 1, value, end ? (size_t)(end - value) : strlen(value));
        out[got > 0 ? got : 0] = 0;
        break;
    }
    return out;
}
static void console_files(const char* folder) {
    static const char* const dirs[] = {"/rw",
                                       "/rw/sys",
                                       "/data",
                                       "/data/00000000000000000000000000000000",
                                       "/data/00000000000000000000000000000000/sysdata",
                                       "/data/00000000000000000000000000000000/sysdata/00010032",
                                       "/data/00000000000000000000000000000000/sysdata/00010032/00000000",
                                       "/data/00000000000000000000000000000000/sysdata/00010032/00000000/2"};
    char path[1024];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; ++i) {
        snprintf(path, sizeof path, "%s%s", folder, dirs[i]);
        check(!snx_mkdirs(path), "the console's folder");
    }
    uint8_t lfcs[0x110], info[0x111] = {0}, account[0x60] = {0};
    for (int i = 0; i < 0x110; ++i) lfcs[i] = (uint8_t)(i * 3 + 7);
    memcpy(info + 0x102, "CW412345678", 11);
    snx_st_le32(account + 0x14, 1234567890u);
    for (int i = 0; i < 8; ++i) account[0x42 + 2 * i] = (uint8_t)"Zq8xK2pL"[i];
    snprintf(path, sizeof path, "%s/rw/sys/LocalFriendCodeSeed_B", folder);
    check(!snx_write_file(path, lfcs, sizeof lfcs), "LocalFriendCodeSeed_B");
    snprintf(path, sizeof path, "%s/rw/sys/SecureInfo_A", folder);
    check(!snx_write_file(path, info, sizeof info), "SecureInfo_A");
    snprintf(path, sizeof path, "%s/data/00000000000000000000000000000000/sysdata/00010032/00000000/2/account", folder);
    check(!snx_write_file(path, account, sizeof account), "the account file");
}

static void login(SnxCtrProcess* p, uint32_t frd, const char* folder) {
    /* offline, or with no console files: the login does not go out, its event signalled at once, NASC's result 0 */
    g_settings.network = 1;
    snprintf(g_settings.ctr_online_service, sizeof g_settings.ctr_online_service, "pretendo");
    g_settings.ctr_console_files[0] = 0;
    uint32_t event = event_new(p);
    uint32_t request[14] = {0x00280244u, 0x00001234u};
    request[2] = 'S' | 'N' << 16, request[3] = 'X';
    request[8] = 3, request[9] = 4, request[10] = 0x20, request[12] = 0, request[13] = event;
    check(send(p, frd, request, 14) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1) && signaled(p, event),
          "RequestGameAuthentication with no console files answers at once");
    receive_buffer(p, 0, RECEIVED, 0x138);
    const uint32_t data[] = {0x00290000u};
    check(send(p, frd, data, 1) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1) && ctr_word(p, RECEIVED) == 0,
          "GetGameAuthenticationData of a login that did not go out: NASC's result 0");
    /* the console's files and a NASC here: the LOGIN goes out, and its answer comes back to the game */
    console_files(folder);
    snprintf(g_settings.ctr_console_files, sizeof g_settings.ctr_console_files, "%s", folder);
    Nasc n = {0};
    SnxSockAddr a = {.family = SNX_AF_INET};
    memcpy(a.addr, loopback, 4);
    check(!snx_socket_open(&n.listener, SNX_AF_INET, SNX_SOCK_STREAM, 0) && !snx_socket_bind(n.listener, &a) &&
              !snx_socket_listen(n.listener, 1) && !snx_socket_name(n.listener, &a),
          "the NASC's socket");
    snprintf(g_settings.ctr_nasc_url, sizeof g_settings.ctr_nasc_url, "http://127.0.0.1:%u/ac/",
             (unsigned)(a.port >> 8 | (a.port & 0xff) << 8));
    check(!plat_thread(nasc_serve, &n, 0), "the NASC's thread");
    p->objects[snx_ctr_handle_object(p, event)].data.event.signaled = 0;
    check(send(p, frd, request, 14) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1), "RequestGameAuthentication");
    for (int tries = 0; tries < 1000 && !signaled(p, event); ++tries) {
        snx_ctr_frd_poll(p);
        if (!signaled(p, event)) plat_sleep_ns(10000000);
    }
    check(signaled(p, event) && atomic_load(&n.done), "the login's event, once NASC answered");
    char value[512];
    check(!strcmp(field(n.form, "action", value, sizeof value), "LOGIN"), "the form's action");
    check(!strcmp(field(n.form, "userid", value, sizeof value), "1234567890"), "the form's userid");
    check(!strcmp(field(n.form, "uidhmac", value, sizeof value), "Zq8xK2pL"), "the form's uidhmac");
    check(!strcmp(field(n.form, "gameid", value, sizeof value), "00001234"), "the form's gameid");
    check(!strcmp(field(n.form, "sdkver", value, sizeof value), "003004"), "the form's sdkver");
    check(!strcmp(field(n.form, "csnum", value, sizeof value), "CW412345678"), "the form's csnum");
    check(send(p, frd, data, 1) == SNX_CTR_SVC_RETURN && !ctr_command_word(p, 1), "GetGameAuthenticationData");
    uint8_t auth[0x138];
    bytes_get(p, RECEIVED, auth, sizeof auth);
    check(snx_ld_le32(auth) == 1 && snx_ld_le32(auth + 4) == 200, "NASC's result 001 and HTTP 200");
    check(!strcmp((char*)auth + 8, "127.0.0.1") && snx_ld_le16(auth + 0x28) == 60123, "the game server");
    check(!strcmp((char*)auth + 0x30, "VE9LRU4tT0Y-U0NBUkxFVA**"), "the token, as NASC sent it");
    snx_socket_unref(n.listener);
}

int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x10000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)0);
    check(p != NULL, "the process");
    check(snx_net_init() == 0, "the network stack");
    g_settings.network = 1;
    g_settings.airplane_mode = 0;
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0}; /* srv's RegisterClient, before GetServiceHandle */
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    sockets(p, ctr_service_handle(p, srv, "soc:U"));
    connection(p, ctr_service_handle(p, srv, "ac:u"));
    char* folder = snx_temp_dir(NULL);
    check(folder != NULL, "a folder for the console's files");
    login(p, ctr_service_handle(p, srv, "frd:u"), folder);
    snx_ctr_process_free(p);
    (void)snx_remove_tree(folder);
    free(folder);
    puts("3ds_online: soc:U, ac:u and frd:u answer as the console's");
    return 0;
}
