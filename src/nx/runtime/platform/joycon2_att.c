/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ATT's PDUs for a Joy-Con 2 (joycon2_att.h); the link that sends them is joycon2_link.c */
#include "nx/runtime/platform/joycon2_att.h"
#include "common/utilities/endian.h"
#include <string.h>

/* the handles that take a write without response, and the CCCDs */
static int cmd_handle(u16 h) { return h == JC2_H_VIBRATION || h == JC2_H_COMMAND || h == JC2_H_VIBRATION_CMD; }
static int cccd_handle(u16 h) {
    return h == JC2_H_COMMON_CCCD || h == JC2_H_NATIVE_CCCD || h == JC2_H_REPLY_CCCD || h == JC2_H_REPLY2_CCCD;
}

size_t jc2_att_mtu_req(u8* out, size_t size, u16 mtu) {
    if (size < 3) return 0;
    out[0] = JC2_ATT_MTU_REQ;
    snx_st_le16(out + 1, mtu);
    return 3;
}
size_t jc2_att_write_cmd(u8* out, size_t size, u16 handle, const u8* data, size_t n) {
    if (!cmd_handle(handle) || !jc2_handle_writable(handle) || size < 3 + n) return 0;
    out[0] = JC2_ATT_WRITE_CMD;
    snx_st_le16(out + 1, handle);
    memcpy(out + 3, data, n);
    return 3 + n;
}
size_t jc2_att_write_cccd(u8* out, size_t size, u16 cccd, int on) {
    if (!cccd_handle(cccd) || !jc2_handle_writable(cccd) || size < 5) return 0;
    out[0] = JC2_ATT_WRITE_REQ;
    snx_st_le16(out + 1, cccd);
    snx_st_le16(out + 3, on ? 1 : 0); /* notifications */
    return 5;
}
int jc2_att_parse(const u8* p, size_t n, Jc2Att* a) {
    if (!n) return 0;
    *a = (Jc2Att){0};
    a->op = p[0];
    switch (a->op) {
        case JC2_ATT_ERROR:
            if (n < 5) return 0;
            a->request = p[1];
            a->handle = snx_ld_le16(p + 2);
            a->error = p[4];
            return 1;
        case JC2_ATT_MTU_REQ:
        case JC2_ATT_MTU_RSP:
            if (n < 3) return 0;
            a->mtu = snx_ld_le16(p + 1);
            return 1;
        case JC2_ATT_NOTIFY:
        case JC2_ATT_INDICATE:
            if (n < 3) return 0;
            a->handle = snx_ld_le16(p + 1);
            a->value = p + 3;
            a->n = n - 3;
            return 1;
        default: return 1;
    }
}
/* a request is an opcode with bit 6 (the command flag) and bit 0 clear, other than a response's; ATT's own list */
static int is_request(u8 op) {
    switch (op) {
        case 0x02: case 0x04: case 0x06: case 0x08: case 0x0a: case 0x0c: case 0x0e: case 0x10: case 0x12: case 0x16:
        case 0x18: case 0x20: return 1;
        default: return 0;
    }
}
size_t jc2_att_answer(const Jc2Att* a, u8* out, size_t size, u16 mtu) {
    if (a->op == JC2_ATT_MTU_REQ) {
        if (size < 3) return 0;
        out[0] = JC2_ATT_MTU_RSP;
        snx_st_le16(out + 1, mtu);
        return 3;
    }
    if (a->op == JC2_ATT_INDICATE) {
        if (size < 1) return 0;
        out[0] = JC2_ATT_CONFIRM;
        return 1;
    }
    if (!is_request(a->op) || size < 5) return 0;
    out[0] = JC2_ATT_ERROR;
    out[1] = a->op;
    out[2] = out[3] = 0;
    out[4] = 0x06; /* request not supported: the host is a client only */
    return 5;
}
