/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The few ATT PDUs a Joy-Con 2 host needs (Bluetooth Core, Vol 3 Part F), as bytes: the MTU exchange, a write
   without response to the vibration and command handles, a CCCD write, and what the controller sends (notifications,
   the responses, an indication or a request of its own). Writes take only the handles joycon2_proto.h allows. Then
   the link: an LE L2CAP socket on the ATT channel, connected unpaired (Linux; elsewhere it opens nothing) */
#ifndef SNX_JOYCON2_ATT_H
#define SNX_JOYCON2_ATT_H
#include "nx/runtime/platform/joycon2_proto.h"

#define JC2_ATT_MTU 512u /* the console asks for it, and takes no less */
enum {
    JC2_ATT_ERROR = 0x01,
    JC2_ATT_MTU_REQ = 0x02,
    JC2_ATT_MTU_RSP = 0x03,
    JC2_ATT_WRITE_REQ = 0x12,
    JC2_ATT_WRITE_RSP = 0x13,
    JC2_ATT_NOTIFY = 0x1b,
    JC2_ATT_INDICATE = 0x1d,
    JC2_ATT_CONFIRM = 0x1e,
    JC2_ATT_WRITE_CMD = 0x52
};
size_t jc2_att_mtu_req(u8* out, size_t size, u16 mtu);
/* a write without response: only to the vibration and command handles; 0 for another handle, or no room */
size_t jc2_att_write_cmd(u8* out, size_t size, u16 handle, const u8* data, size_t n);
/* a write request of a CCCD, notifications on or off: only the CCCDs of joycon2_proto.h */
size_t jc2_att_write_cccd(u8* out, size_t size, u16 cccd, int on);
/* a PDU the controller sent */
typedef struct {
    u8 op;
    u16 handle;      /* a notification's, an indication's, an error's */
    const u8* value; /* a notification's or indication's bytes */
    size_t n;
    u16 mtu;         /* an MTU request's or response's */
    u8 request, error; /* an error response's: the request it refuses, and why */
} Jc2Att;
int jc2_att_parse(const u8* p, size_t n, Jc2Att* a); /* 0 when it is too short for its opcode */
/* what the host answers a PDU of the controller's: an MTU request its own MTU, an indication its confirmation, any
   other request "not supported"; 0 for a PDU that takes no answer */
size_t jc2_att_answer(const Jc2Att* a, u8* out, size_t size, u16 mtu);

/* ---- the link */
typedef struct {
    int fd; /* -1 for none */
    u16 mtu;
    int err; /* why jc2_link_open failed: an errno value (ETIMEDOUT when the connection didn't come in time) */
} Jc2Link;
/* connect to address (as written, AA:BB:..., most significant first) on the ATT channel, unpaired, and exchange the
   MTU. 0 when it is up; on Linux only */
int jc2_link_open(Jc2Link* l, const u8 address[6], int random_address, int timeout_ms);
void jc2_link_close(Jc2Link* l);
/* Jc2Transport's two writes; the CCCD write waits up to 2 s for its response, dropping the notifications meanwhile */
int jc2_link_write(void* link, u16 handle, const u8* data, size_t n);
int jc2_link_notify(void* link, u16 cccd, int on);
/* waits up to timeout_ms for one PDU: a notification goes to on_notify, a request is answered. 1 for a PDU, 0 for
   none, -1 when the link is down */
int jc2_link_poll(Jc2Link* l, int timeout_ms, void (*on_notify)(void* user, u16 handle, const u8* p, size_t n),
                  void* user);
#endif
