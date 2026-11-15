/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2's link (joycon2_att.h's Jc2Link): an LE L2CAP socket on the ATT channel, connected unpaired. Written
   from the kernel's socket interface and the public notes (trevlars' switch2-controllers-linux connects the same way:
   channel 4, BT_SECURITY_LOW, no pairing); it connected a left and a right Joy-Con 2. Linux only, nothing elsewhere */
#include "nx/runtime/platform/joycon2_att.h"
#include "common/utilities/endian.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* the kernel's Bluetooth socket interface (include/net/bluetooth/bluetooth.h, l2cap.h), declared here so the build
   needs no BlueZ headers */
#define BT_AF            31 /* AF_BLUETOOTH */
#define BT_PROTO_L2CAP   0
#define BT_SOL           274 /* SOL_BLUETOOTH */
#define BT_OPT_SECURITY  4
#define BT_OPT_RCVMTU    13
#define BT_SEC_LOW       1
#define BT_ADDR_LE_PUBLIC 1
#define BT_ADDR_LE_RANDOM 2
#define ATT_CID          4
struct bt_sockaddr_l2 {
    sa_family_t family;
    unsigned short psm;
    u8 address[6]; /* least significant first */
    unsigned short cid;
    u8 address_type;
};
struct bt_sec {
    u8 level, key_size;
};

static int link_send(Jc2Link* l, const u8* p, size_t n) {
    if (l->fd < 0 || n > l->mtu) return -1;
    return send(l->fd, p, n, MSG_NOSIGNAL) == (ssize_t)n ? 0 : -1;
}
/* one PDU within timeout_ms into buf: its size, 0 for none, -1 when the link is down */
static int link_recv(Jc2Link* l, u8* buf, size_t size, int timeout_ms) {
    struct pollfd pfd = {l->fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (!r) return 0;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    ssize_t n = recv(l->fd, buf, size, 0);
    return n > 0 ? (int)n : -1;
}
int jc2_link_open(Jc2Link* l, const u8 address[6], int random_address, int timeout_ms) {
    struct bt_sockaddr_l2 local = {0}, peer = {0};
    struct bt_sec sec = {BT_SEC_LOW, 0};
    int mtu = JC2_ATT_MTU, err = 0;
    socklen_t len = sizeof err;
    u8 buf[JC2_ATT_MTU];
    l->fd = socket(BT_AF, SOCK_SEQPACKET | SOCK_CLOEXEC, BT_PROTO_L2CAP);
    l->mtu = 23; /* ATT's default until the exchange */
    l->err = 0;
    if (l->fd < 0) {
        l->err = errno;
        return -1;
    }
    local.family = BT_AF;
    local.cid = snx_le16(ATT_CID);
    local.address_type = BT_ADDR_LE_PUBLIC;
    peer.family = BT_AF;
    peer.cid = snx_le16(ATT_CID);
    peer.address_type = random_address ? BT_ADDR_LE_RANDOM : BT_ADDR_LE_PUBLIC;
    for (int i = 0; i < 6; i++) peer.address[i] = address[5 - i];
    /* no pairing: the controller drops a link whose host starts SMP */
    if (bind(l->fd, (struct sockaddr*)&local, sizeof local) || setsockopt(l->fd, BT_SOL, BT_OPT_SECURITY, &sec, sizeof sec)) {
        l->err = errno;
        goto fail;
    }
    setsockopt(l->fd, BT_SOL, BT_OPT_RCVMTU, &mtu, sizeof mtu); /* a kernel that has no such option keeps its own */
    fcntl(l->fd, F_SETFL, fcntl(l->fd, F_GETFL) | O_NONBLOCK);
    if (connect(l->fd, (struct sockaddr*)&peer, sizeof peer) && errno != EINPROGRESS) {
        l->err = errno;
        goto fail;
    }
    {
        struct pollfd pfd = {l->fd, POLLOUT, 0};
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready != 1) {
            l->err = ready ? errno : ETIMEDOUT;
            goto fail;
        }
        if (getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
            l->err = err ? err : errno;
            goto fail;
        }
    }
    /* the MTU exchange: the 63-byte reports need more than ATT's 23 */
    size_t n = jc2_att_mtu_req(buf, sizeof buf, JC2_ATT_MTU);
    if (link_send(l, buf, n)) goto fail;
    for (int tries = 0; tries < 20; tries++) {
        Jc2Att a;
        int got = link_recv(l, buf, sizeof buf, 100);
        if (got < 0) {
            l->err = errno ? errno : EIO;
            goto fail;
        }
        if (!got || !jc2_att_parse(buf, (size_t)got, &a)) continue;
        if (a.op == JC2_ATT_MTU_RSP) {
            l->mtu = a.mtu < JC2_ATT_MTU ? (a.mtu < 23 ? 23 : a.mtu) : JC2_ATT_MTU;
            return 0;
        }
        if (a.op == JC2_ATT_ERROR) return 0; /* no exchange: ATT's default */
    }
fail:
    jc2_link_close(l);
    return -1;
}
void jc2_link_close(Jc2Link* l) {
    if (l->fd >= 0) close(l->fd);
    l->fd = -1;
}
int jc2_link_write(void* link, u16 handle, const u8* data, size_t n) {
    u8 buf[JC2_ATT_MTU];
    size_t m = jc2_att_write_cmd(buf, sizeof buf, handle, data, n);
    return m ? link_send((Jc2Link*)link, buf, m) : -1;
}
int jc2_link_notify(void* link, u16 cccd, int on) {
    Jc2Link* l = (Jc2Link*)link;
    u8 buf[JC2_ATT_MTU];
    size_t m = jc2_att_write_cccd(buf, sizeof buf, cccd, on);
    if (!m || link_send(l, buf, m)) return -1;
    for (int tries = 0; tries < 20; tries++) {
        Jc2Att a;
        int got = link_recv(l, buf, sizeof buf, 100);
        if (got < 0) return -1;
        if (!got || !jc2_att_parse(buf, (size_t)got, &a)) continue;
        if (a.op == JC2_ATT_WRITE_RSP) return 0;
        if (a.op == JC2_ATT_ERROR && a.request == JC2_ATT_WRITE_REQ) return -1;
        u8 out[8];
        size_t k = jc2_att_answer(&a, out, sizeof out, l->mtu);
        if (k && link_send(l, out, k)) return -1;
    }
    return -1;
}
int jc2_link_poll(Jc2Link* l, int timeout_ms, void (*on_notify)(void* user, u16 handle, const u8* p, size_t n),
                  void* user) {
    u8 buf[JC2_ATT_MTU], out[8];
    Jc2Att a;
    int got = link_recv(l, buf, sizeof buf, timeout_ms);
    if (got < 0 && l->fd >= 0) { /* why it went down: the socket's error (a supervision timeout, the peer's end) */
        int err = 0;
        socklen_t len = sizeof err;
        l->err = getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &err, &len) ? errno : err ? err : ENOTCONN;
    }
    if (got <= 0) return got;
    if (!jc2_att_parse(buf, (size_t)got, &a)) return 1;
    if (a.op == JC2_ATT_NOTIFY || a.op == JC2_ATT_INDICATE) on_notify(user, a.handle, a.value, a.n);
    size_t k = jc2_att_answer(&a, out, sizeof out, l->mtu);
    if (k && link_send(l, out, k)) return -1;
    return 1;
}
#else
int jc2_link_open(Jc2Link* l, const u8 address[6], int random_address, int timeout_ms) {
    (void)address, (void)random_address, (void)timeout_ms;
    l->fd = -1;
    l->mtu = 23;
    return -1;
}
void jc2_link_close(Jc2Link* l) { l->fd = -1; }
int jc2_link_write(void* link, u16 handle, const u8* data, size_t n) {
    (void)link, (void)handle, (void)data, (void)n;
    return -1;
}
int jc2_link_notify(void* link, u16 cccd, int on) {
    (void)link, (void)cccd, (void)on;
    return -1;
}
int jc2_link_poll(Jc2Link* l, int timeout_ms, void (*on_notify)(void* user, u16 handle, const u8* p, size_t n),
                  void* user) {
    (void)l, (void)timeout_ms, (void)on_notify, (void)user;
    return -1;
}
#endif
