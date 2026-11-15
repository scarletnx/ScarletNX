/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2's protocol as bytes (joycon2_proto.h) */
#include "nx/runtime/platform/joycon2_proto.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <string.h>

int jc2_handle_writable(u16 handle) {
    switch (handle) {
        case JC2_H_COMMON_CCCD:
        case JC2_H_NATIVE_CCCD:
        case JC2_H_VIBRATION:
        case JC2_H_COMMAND:
        case JC2_H_VIBRATION_CMD:
        case JC2_H_REPLY_CCCD:
        case JC2_H_REPLY2_CCCD: return 1;
        default: return 0;
    }
}

/* ---- commands */
int jc2_cmd_allowed(u8 cmd, u8 sub) {
    switch (cmd) {
        case JC2_CMD_FLASH: return sub == 0x04;
        case JC2_CMD_LED: return sub == 0x07 || sub == 0x08;
        case JC2_CMD_VIBRATION: return sub == 0x02 || sub == 0x08;
        case JC2_CMD_FEATURES: return sub == 0x02 || sub == 0x04 || sub == 0x05;
        case JC2_CMD_VERSION: return sub == 0x01;
        case JC2_CMD_IMU: return sub == 0x01 || sub == 0x03;
        case JC2_CMD_MOUSE: return sub == 0x01;
        default: return 0;
    }
}
size_t jc2_cmd(u8* out, size_t size, u8 cmd, u8 sub, const u8* payload, size_t n) {
    if (!jc2_cmd_allowed(cmd, sub) || n > JC2_CMD_PAYLOAD || size < JC2_CMD_HEADER + n) return 0;
    out[0] = cmd;
    out[1] = 0x91; /* a request */
    out[2] = 0x01; /* over Bluetooth */
    out[3] = sub;
    snx_st_be16(out + 4, (u16)n);
    out[6] = out[7] = 0;
    if (n) memcpy(out + JC2_CMD_HEADER, payload, n);
    return JC2_CMD_HEADER + n;
}
size_t jc2_cmd_flash_read(u8* out, size_t size, u32 address, u8 n) {
    u8 p[8] = {n, 0x7e, 0, 0};
    if (!n || n > JC2_FLASH_READ_MAX || (address & 0xfffu) + n > 0x1000u) return 0;
    snx_st_le32(p + 4, address);
    return jc2_cmd(out, size, JC2_CMD_FLASH, 0x04, p, sizeof p);
}
size_t jc2_cmd_features(u8* out, size_t size, u8 sub, u8 mask) {
    const u8 p[4] = {mask, 0, 0, 0};
    return sub == 0x02 || sub == 0x04 || sub == 0x05 ? jc2_cmd(out, size, JC2_CMD_FEATURES, sub, p, sizeof p) : 0;
}

int jc2_reply(const u8* p, size_t n, Jc2Reply* r) {
    if (n < JC2_CMD_HEADER) return 0;
    r->cmd = p[0];
    r->status = p[1];
    r->sub = p[3];
    r->data = p + JC2_CMD_HEADER;
    r->size = n - JC2_CMD_HEADER;
    return 1;
}
/* the data: its length at 0, the address at 4, the bytes from 8 */
const u8* jc2_reply_flash(const Jc2Reply* r, u32 address, u8 n) {
    if (r->cmd != JC2_CMD_FLASH || r->sub != 0x04 || r->status != 1 || r->size < 8u + n) return NULL;
    if (r->data[0] != n || snx_ld_le32(r->data + 4) != address) return NULL;
    return r->data + 8;
}

/* ---- flash */
int jc2_info(const u8* p, size_t n, Jc2Info* info) {
    if (n < 0x22) return 0;
    *info = (Jc2Info){0};
    for (int i = 0; i < 16; i++) /* ASCII, an unwritten byte 0xFF */
        info->serial[i] = p[2 + i] >= 0x20 && p[2 + i] < 0x7f ? (char)p[2 + i] : 0;
    info->vendor = snx_ld_le16(p + 0x12);
    info->product = snx_ld_le16(p + 0x14);
    memcpy(info->body, p + 0x19, 3);
    memcpy(info->buttons, p + 0x1c, 3);
    return 1;
}
/* three pairs of 12-bit values, X in the low 12 bits of each 3 bytes */
static void unpack12(const u8* b, u16* x, u16* y) {
    *x = (u16)(b[0] | (b[1] & 0x0f) << 8);
    *y = (u16)(b[1] >> 4 | b[2] << 4);
}
int jc2_stick_cal(const u8 b[9], Jc2StickCal* c) {
    int unset = 1;
    for (int i = 0; i < 9; i++) unset &= b[i] == 0xff;
    if (unset) return 0;
    unpack12(b, &c->center[0], &c->center[1]);
    unpack12(b + 3, &c->above[0], &c->above[1]);
    unpack12(b + 6, &c->below[0], &c->below[1]);
    return c->above[0] && c->above[1] && c->below[0] && c->below[1];
}
int jc2_user_stick_cal(const u8 b[11], Jc2StickCal* c) { return b[0] == 0xb2 && b[1] == 0xa1 && jc2_stick_cal(b + 2, c); }
i32 jc2_stick_axis(u16 raw, const Jc2StickCal* c, int axis) {
    i32 d = (i32)raw - (i32)c->center[axis];
    i32 range = d >= 0 ? c->above[axis] : c->below[axis];
    if (!range) return 0;
    i64 v = (i64)d * 32767 / range;
    return (i32)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}
/* a float, then the three offsets: the console refuses offsets past 44 degrees/s */
int jc2_gyro_cal(const u8 p[16], float offset[3]) {
    for (int i = 0; i < 3; i++) {
        offset[i] = snx_ld_lef32(p + 4 + 4 * i);
        if (!(fabsf(offset[i]) < 44.0f / 57.2958f)) { /* a NaN too */
            offset[0] = offset[1] = offset[2] = 0;
            return 0;
        }
    }
    return 1;
}

/* ---- the common report */
/* bytes 4 to 7, a bit each from bit 0: the Npad's button (0..27), or 32 + a JC2_X_* bit's index, or -1 */
static const signed char REPORT_BUTTONS[4][8] = {
    {3, 2, 1, 0, 27, 26, 7, 9},        /* Y X B A, the right Joy-Con's SR SL, R ZR */
    {11, 10, 5, 4, 32, 33, 34, -1},    /* Minus Plus, StickR StickL, Home Capture C */
    {15, 13, 14, 12, 25, 24, 6, 8},    /* Down Up Right Left, the left Joy-Con's SR SL, L ZL */
    {36, 35, -1, -1, -1, -1, -1, -1}}; /* GR GL */
int jc2_report(const u8* p, size_t n, u32 mask, Jc2Report* r) {
    if (n < JC2_REPORT_SIZE) return 0;
    *r = (Jc2Report){0};
    r->counter = snx_ld_le32(p);
    if (mask & JC2_F_BUTTONS)
        for (int byte = 0; byte < 4; byte++)
            for (int bit = 0; bit < 8; bit++) {
                int to = REPORT_BUTTONS[byte][bit];
                if (to < 0 || !(p[4 + byte] >> bit & 1)) continue;
                if (to < 32)
                    r->buttons |= 1ull << to;
                else
                    r->extra |= (u8)(1u << (to - 32));
            }
    if (mask & JC2_F_STICKS) {
        unpack12(p + 0x0a, &r->stick[0][0], &r->stick[0][1]);
        unpack12(p + 0x0d, &r->stick[1][0], &r->stick[1][1]);
    }
    if (mask & JC2_F_MOUSE) {
        r->mouse[0] = snx_ld_le16(p + 0x10);
        r->mouse[1] = snx_ld_le16(p + 0x12);
        r->mouse_surface = p[0x14];
    } else
        r->mouse_surface = 0xff;
    r->battery_mv = snx_ld_le16(p + 0x1f);
    if (mask & JC2_F_IMU) {
        r->imu_us = snx_ld_le32(p + 0x2a);
        for (int i = 0; i < 3; i++) {
            r->accel[i] = (i16)snx_sext32(snx_ld_le16(p + 0x30 + 2 * i), 16);
            r->gyro[i] = (i16)snx_sext32(snx_ld_le16(p + 0x36 + 2 * i), 16);
        }
    }
    return 1;
}
i32 jc2_mouse_delta(u16 now, u16 prev) { return snx_sext32((u16)(now - prev), 16); }
/* the plain sample's ranges: +-8 g and +-2000 degrees/s over 16 bits (the IMU's own report of them, 11/03) */
void jc2_motion(const Jc2Report* r, const float gyro_offset[3], float accel[3], float gyro[3]) {
    for (int i = 0; i < 3; i++) {
        /* The IMU reads +1 g on Z lying face up (X toward the rail, Y toward the trigger, Z out of the face, right-
           handed; measured). The console hands its six-axis filter the IMU's acceleration negated and its
           angular velocity as it is, on the same axes, for every Switch 2 pad, left and right alike (Switch 1 pads
           get the same sign rule after their own rotation), and the filter changes no axis. So the state reads -1 g
           on Z lying face up, as gamepad.c's SDL pads do. Assumed: the common report's axes are those of the native
           stream hid reads */
        accel[i] = -(float)r->accel[i] / 4096.0f;
        gyro[i] = (float)r->gyro[i] / 16.4f * (3.14159265f / 180.0f) - gyro_offset[i];
    }
}

/* ---- the advert: 01 00 03, vendor, product, 00 01, flags, the host's address */
int jc2_advert(const u8* p, size_t n, Jc2Advert* a) {
    if (n < 16 || p[0] != 0x01 || p[1] != 0x00 || p[2] != 0x03 || snx_ld_le16(p + 3) != JC2_VENDOR) return 0;
    a->product = snx_ld_le16(p + 5);
    if (a->product != JC2_PID_LEFT && a->product != JC2_PID_RIGHT && a->product != JC2_PID_PRO) return 0;
    a->wake = p[9] >> 7 & 1;
    memcpy(a->host, p + 10, 6);
    a->syncing = 1;
    for (int i = 0; i < 6; i++) a->syncing &= !a->host[i];
    return 1;
}

/* ---- HD rumble */
u16 jc2_rumble_freq(float hz) {
    hz = snx_clamp_float(hz, 40.0f, 3500.0f);
    if (!(hz == hz)) hz = 160.0f; /* a NaN: the neutral low band */
    return (u16)lrintf(96.0f * log2f(hz) - 318.01f);
}
u16 jc2_rumble_amp(float a) {
    if (!(a > 0.0f)) return 0; /* a NaN too */
    return (u16)lrintf(snx_clamp_float(a, 0.0f, 1.0f) * 1023.0f);
}
void jc2_rumble_pack(u8 out[5], u16 freq_low, u16 amp_low, u16 freq_high, u16 amp_high) {
    u64 s = (u64)(freq_low & 0x3ffu) | (u64)(amp_low & 0x3ffu) << 10 | (u64)(freq_high & 0x3ffu) << 20 |
            (u64)(amp_high & 0x3ffu) << 30;
    for (int i = 0; i < 5; i++) out[i] = (u8)(s >> 8 * i);
}
void jc2_rumble_limit(Jc2Vibration* v) {
    float lo = v->amp_low > 0.0f ? v->amp_low : 0.0f, hi = v->amp_high > 0.0f ? v->amp_high : 0.0f; /* NaN: 0 */
    if (lo + hi > 0.98f) {
        float k = 0.98f / (lo + hi);
        lo *= k;
        hi *= k;
    }
    v->amp_low = lo < JC2_RUMBLE_CAP ? lo : JC2_RUMBLE_CAP;
    v->amp_high = hi < JC2_RUMBLE_CAP ? hi : JC2_RUMBLE_CAP;
}
/* one actuator's 16-byte block at out: the state byte, then v (limited) in all three samples */
static void rumble_block(u8* out, const Jc2Vibration* v, unsigned counter) {
    Jc2Vibration l = *v;
    u8 sample[5];
    jc2_rumble_limit(&l);
    jc2_rumble_pack(sample, jc2_rumble_freq(l.freq_low), jc2_rumble_amp(l.amp_low), jc2_rumble_freq(l.freq_high),
                    jc2_rumble_amp(l.amp_high));
    out[0] = (u8)(0x40u | 3u << 4 | (counter & 0x0fu)); /* three samples */
    for (int i = 0; i < 3; i++) memcpy(out + 1 + 5 * i, sample, 5);
}
void jc2_rumble_packet(u8 out[JC2_RUMBLE_PACKET], const Jc2Vibration* v, unsigned counter) {
    memset(out, 0, JC2_RUMBLE_PACKET);
    rumble_block(out + 1, v, counter);
}
void jc2_rumble_packet_pro(u8 out[JC2_RUMBLE_PACKET], const Jc2Vibration* left, const Jc2Vibration* right,
                           unsigned counter) {
    memset(out, 0, JC2_RUMBLE_PACKET);
    rumble_block(out + 1, left, counter);
    rumble_block(out + 17, right, counter);
}
