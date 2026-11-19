/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The resolver against a DNS server on this host that answers as a WFC service's does: its own address for the
   service's names, no such name for the rest, a stray datagram first to be passed over */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif
#ifdef _WIN32
#include <winsock2.h> /* before thread.h's windows.h */
#include <ws2tcpip.h>
#endif
#include "common/online/dns.h"
#include "common/online/wfc.h"
#include "online_check.h"
#include "online_thread.h"
#include <stdint.h>
#ifdef _WIN32
typedef SOCKET Fd;
#define closefd closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Fd;
#define closefd close
#endif

typedef struct Server {
    Fd fd;
    uint16_t port;
    int answers;  /* the queries it answers before it ends */
    int silent;   /* it reads and never answers */
    int delay_ms; /* how long it waits before it answers */
} Server;

static void pause_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static int server_open(Server* s) {
    struct sockaddr_in a = {0};
    socklen_t length = sizeof a;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (bind(s->fd, (struct sockaddr*)&a, sizeof a) || getsockname(s->fd, (struct sockaddr*)&a, &length)) return -1;
    s->port = ntohs(a.sin_port);
    return 0;
}

SNX_ONLINE_THREAD_MAIN(serve, arg) {
    Server* s = (Server*)arg;
    static const uint8_t wfc[1][4] = {{127, 0, 0, 2}};
    for (int n = 0; n < s->answers; ++n) {
        uint8_t q[SNX_ONLINE_DNS_MESSAGE], r[SNX_ONLINE_DNS_MESSAGE];
        struct sockaddr_in from;
        socklen_t length = sizeof from;
        char name[SNX_ONLINE_DNS_NAME];
        int got = (int)recvfrom(s->fd, (char*)q, sizeof q, 0, (struct sockaddr*)&from, &length);
        if (got <= 0) break;
        if (s->silent || snx_online_dns_question(q, (size_t)got, name, NULL)) continue;
        if (s->delay_ms) pause_ms(s->delay_ms);
        int service = strlen(name) >= 16 && !strcmp(name + strlen(name) - 16, "nintendowifi.net");
        size_t rl = snx_online_dns_reply(r, sizeof r, q, (size_t)got, wfc, service ? 1 : 0, 60);
        uint8_t stray[SNX_ONLINE_DNS_MESSAGE];
        memcpy(stray, r, rl);
        stray[0] ^= 0x55; /* an earlier query's late reply: another id */
        sendto(s->fd, (const char*)stray, (int)rl, 0, (struct sockaddr*)&from, length);
        sendto(s->fd, (const char*)r, (int)rl, 0, (struct sockaddr*)&from, length);
    }
    SNX_ONLINE_THREAD_END;
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    static const uint8_t local[4] = {127, 0, 0, 1};
    uint8_t addr[4][4];
    char why[512] = "";
    Server s = {0};
    SnxOnlineThread t;
    CHECK_INT(server_open(&s), 0);
    s.answers = 2;
    CHECK_INT(snx_online_thread_start(&t, serve, &s), 0);
    CHECK_INT(snx_online_dns_resolve(local, s.port, "nas.nintendowifi.net", addr, 4, 2000, why, sizeof why), 1);
    CHECK(addr[0][0] == 127 && addr[0][1] == 0 && addr[0][2] == 0 && addr[0][3] == 2);
    CHECK_INT(snx_online_dns_resolve(local, s.port, "example.org", addr, 4, 2000, why, sizeof why), 0);
    snx_online_thread_join(t);
    closefd(s.fd);

    /* a server that never answers: the wait ends at its time, and says it */
    Server quiet = {0};
    CHECK_INT(server_open(&quiet), 0);
    quiet.answers = 1;
    quiet.silent = 1;
    CHECK_INT(snx_online_thread_start(&t, serve, &quiet), 0);
    CHECK_INT(snx_online_dns_resolve(local, quiet.port, "nas.nintendowifi.net", addr, 4, 300, why, sizeof why), -1);
    CHECK_HAS(why, "no reply in 300 ms");
    CHECK_HAS(why, "127.0.0.1:");
    CHECK_HAS(why, "nas.nintendowifi.net");
    snx_online_thread_join(t);
    closefd(quiet.fd);

    /* auto: the fastest of three, one answering at once, one 150 ms late, one never; then none answering */
    Server fast = {0}, slow = {0}, mute = {0};
    SnxOnlineThread tf, ts, tm;
    CHECK_INT(server_open(&fast) | server_open(&slow) | server_open(&mute), 0);
    fast.answers = slow.answers = mute.answers = 1;
    slow.delay_ms = 150;
    mute.silent = 1;
    CHECK_INT(snx_online_thread_start(&tf, serve, &fast) | snx_online_thread_start(&ts, serve, &slow) |
                  snx_online_thread_start(&tm, serve, &mute),
              0);
    const uint8_t three[3][4] = {{127, 0, 0, 1}, {127, 0, 0, 1}, {127, 0, 0, 1}};
    const uint16_t ports[3] = {slow.port, mute.port, fast.port};
    int ms[3] = {0, 0, 0};
    CHECK_INT(snx_online_wfc_fastest(three, ports, 3, 1000, ms, why, sizeof why), 2);
    CHECK(ms[2] >= 0 && ms[2] < 150 && ms[0] >= 150 && ms[0] < 1000 && ms[1] == -1);
    snx_online_thread_join(tf);
    snx_online_thread_join(ts);
    snx_online_thread_join(tm);
    closefd(fast.fd);
    closefd(slow.fd);
    closefd(mute.fd);
    Server quiet2 = {0};
    CHECK_INT(server_open(&quiet2), 0);
    quiet2.answers = 1;
    quiet2.silent = 1;
    CHECK_INT(snx_online_thread_start(&t, serve, &quiet2), 0);
    const uint16_t one_port[1] = {quiet2.port};
    CHECK_INT(snx_online_wfc_fastest(three, one_port, 1, 200, ms, why, sizeof why), -1);
    CHECK(ms[0] == -1);
    CHECK_HAS(why, "none of the 1 servers gave an address for nas.nintendowifi.net within 200 ms");
    snx_online_thread_join(t);
    closefd(quiet2.fd);

    CHECK_INT(snx_online_dns_resolve(local, 53, "bad..name", addr, 4, 300, why, sizeof why), -1);
    CHECK_HAS(why, "\"bad..name\" is not a host name");
    CHECK_DONE();
}
