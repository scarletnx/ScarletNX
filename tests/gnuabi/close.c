/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* A descriptor that one thread closes while another waits on it, then a byte from the socket's peer: what Linux
   gives (its file reference keeps the file through the wait). recvfrom takes the byte; ppoll finds the descriptor
   closed at its last look (POLLNVAL); pselect6 reports the file it waited on as readable; epoll_pwait, whose own
   descriptor was closed, reports the socket's byte. Exits 0 */
#include "threading.h"

static u8 stack[65536] __attribute__((aligned(16)));
static volatile u32 ready, ctid;
static volatile s64 result;
static s32 sv[2], epoll;
static u64 seen; /* the call's revents, fd_set word or epoll events */

struct pollfd {
    s32 fd;
    short events, revents;
};

static int waiter(u64 kind) {
    u8 buffer[16];
    s64 forever[2] = {5, 0};
    raise_flag(&ready);
    if (kind == 0) {
        result = syscall6(207, (u64)sv[0], (u64)buffer, sizeof buffer, 0, 0, 0); /* recvfrom */
    } else if (kind == 1) {
        struct pollfd p = {sv[0], 1 /* POLLIN */, 0};
        result = syscall6(73, (u64)&p, 1, (u64)forever, 0, 8, 0); /* ppoll */
        seen = (u64)(unsigned short)p.revents;
    } else if (kind == 2) {
        u64 set[16] = {0};
        set[sv[0] / 64] = 1ul << (sv[0] % 64);
        result = syscall6(72, (u64)sv[0] + 1, (u64)set, 0, 0, (u64)forever, 0); /* pselect6 */
        seen = set[sv[0] / 64] >> (sv[0] % 64) & 1;
    } else {
        u8 event[16] = {0};
        result = syscall6(22, (u64)epoll, (u64)event, 1, 5000, 0, 8); /* epoll_pwait */
        seen = *(u32*)event;
    }
    return 0;
}

/* kind's call waits on another thread, which closes closed and then sends the socket's peer a byte */
static void run(u64 kind, s32* closed) {
    CHECK(syscall6(199, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, (u64)sv, 0, 0) == 0);
    if (kind == 3) {
        u8 event[16] = {1 /* EPOLLIN */};
        CHECK((epoll = (s32)call(20, 0, 0, 0)) >= 0);                          /* epoll_create1 */
        CHECK(syscall6(21, (u64)epoll, 1, (u64)sv[0], (u64)event, 0, 0) == 0); /* EPOLL_CTL_ADD */
    }
    result = -1000;
    seen = 0;
    store(&ready, 0);
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, waiter, kind) > 0);
    await(&ready);
    sleep_ns(100000000);
    CHECK(call(57, (u64)*closed, 0, 0) == 0);
    sleep_ns(100000000);
    CHECK(call(64, (u64)sv[1], (u64) "x", 1) == 1);
    join(&ctid);
    CHECK(call(57, (u64)sv[1], 0, 0) == 0);
    if (kind == 3) CHECK(call(57, (u64)sv[0], 0, 0) == 0);
}

int guest_main(u64* sp) {
    (void)sp;
    run(0, &sv[0]);
    CHECK(result == 1);
    put("close: recvfrom ok\n");
    run(1, &sv[0]);
    CHECK(result == 1 && seen == 0x20 /* POLLNVAL */);
    put("close: ppoll ok\n");
    run(2, &sv[0]);
    CHECK(result == 1 && seen == 1);
    put("close: pselect6 ok\n");
    run(3, &epoll);
    CHECK(result == 1 && seen == 1 /* EPOLLIN */);
    put("close: epoll ok\n");
    return 0;
}
