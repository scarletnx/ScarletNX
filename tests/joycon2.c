/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2's protocol (runtime/platform/joycon2_proto.c) and one controller's setup and input
   (joycon2_session.c), on bytes made here, no Bluetooth: the requests against the bytes that public captures show
   (ndeadly's switch2_controller_research, commands.md and bluetooth_interface.md), the commands and handles the host
   must never send or write, the replies of those captures, the common report's fields and the features that gate them,
   the calibration, the HD rumble's codes against a public frame (joycon2cpp's), the setup's order, retries and
   failures through a transport that records what it would send, and ATT's PDUs (joycon2_att.c) */
#include "nx/runtime/platform/joycon2_att.h"
#include "nx/runtime/platform/joycon2_session.h"
#include "common/utilities/endian.h"
#include "common/platform/ble_scan.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MS 1000000ull
#define S  1000000000ull

/* what the transport was asked to send */
typedef struct {
    u16 handle; /* 0xffff: a CCCD write */
    u16 cccd;
    size_t n;
    u8 data[64];
} Sent;
static struct {
    Sent sent[64];
    int n;
    int fail; /* the next writes fail */
} g_test;

static int fake_write(void* link, u16 handle, const u8* data, size_t n) {
    (void)link;
    assert(jc2_handle_writable(handle)); /* never a handle off the list */
    if (g_test.fail) return -1;
    assert(g_test.n < 64 && n <= 64);
    Sent* s = &g_test.sent[g_test.n++];
    s->handle = handle;
    s->n = n;
    memcpy(s->data, data, n);
    return 0;
}
static int fake_notify(void* link, u16 cccd, int on) {
    (void)link;
    assert(on && (cccd == JC2_H_REPLY_CCCD || cccd == JC2_H_COMMON_CCCD));
    if (g_test.fail) return -1;
    assert(g_test.n < 64);
    g_test.sent[g_test.n].handle = 0xffff;
    g_test.sent[g_test.n].cccd = cccd;
    g_test.sent[g_test.n++].n = 0;
    return 0;
}
static const Jc2Transport T = {NULL, fake_write, fake_notify};

static int same(const u8* a, size_t n, const u8* b, size_t m) { return n == m && !memcmp(a, b, n); }
#define SAME(a, n, ...)                                                                                                \
    do {                                                                                                               \
        static const u8 want_[] = {__VA_ARGS__};                                                                       \
        assert(same(a, n, want_, sizeof want_));                                                                      \
    } while (0)

/* ---- requests, against the captures */
static void test_requests(void) {
    u8 b[64];
    size_t n;
    /* commands.md: a read of 0x10 bytes at 0x13040 */
    n = jc2_cmd_flash_read(b, sizeof b, 0x13040, 0x10);
    SAME(b, n, 0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x10, 0x7e, 0x00, 0x00, 0x40, 0x30, 0x01, 0x00);
    /* bluetooth_interface.md's console setup */
    n = jc2_cmd_flash_read(b, sizeof b, 0x1fc040, 0x40);
    SAME(b, n, 0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7e, 0x00, 0x00, 0x40, 0xc0, 0x1f, 0x00);
    n = jc2_cmd_features(b, sizeof b, 0x02, JC2_F_JOYCON);
    SAME(b, n, 0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00);
    n = jc2_cmd_features(b, sizeof b, 0x04, JC2_F_JOYCON);
    SAME(b, n, 0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00);
    n = jc2_cmd(b, sizeof b, JC2_CMD_VERSION, 0x01, NULL, 0);
    SAME(b, n, 0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00);
    /* the payload's length is 16-bit big-endian: a 0x14-byte payload */
    {
        static const u8 p[20] = {0x01, 0x59, 0x09, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46};
        n = jc2_cmd(b, sizeof b, JC2_CMD_VIBRATION, 0x08, p, sizeof p);
        SAME(b, n, 0x0a, 0x91, 0x01, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0x59, 0x09, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
             0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    }
    /* no room, too much payload, a read past its page or larger than 0x40 */
    assert(!jc2_cmd(b, 8, JC2_CMD_FEATURES, 0x02, b, 4));
    assert(!jc2_cmd(b, sizeof b, JC2_CMD_FEATURES, 0x02, b, JC2_CMD_PAYLOAD + 1));
    assert(!jc2_cmd_flash_read(b, sizeof b, 0x13ff0, 0x20) && jc2_cmd_flash_read(b, sizeof b, 0x13fc0, 0x40));
    assert(!jc2_cmd_flash_read(b, sizeof b, 0x13000, 0x41) && !jc2_cmd_flash_read(b, sizeof b, 0x13000, 0));
    assert(!jc2_cmd_features(b, sizeof b, 0x03, 0x37) && !jc2_cmd_features(b, sizeof b, 0x06, 0x37));
}

/* ---- what the host can never send or write: exactly these commands, these handles */
static void test_refusals(void) {
    static const u8 allowed[][2] = {{0x02, 0x04}, {0x09, 0x07}, {0x09, 0x08}, {0x0a, 0x02}, {0x0a, 0x08},
                                    {0x0c, 0x02}, {0x0c, 0x04}, {0x0c, 0x05}, {0x10, 0x01}, {0x11, 0x01},
                                    {0x11, 0x03}, {0x13, 0x01}};
    u8 b[64];
    for (unsigned cmd = 0; cmd < 256; cmd++)
        for (unsigned sub = 0; sub < 256; sub++) {
            int want = 0;
            for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++)
                want |= allowed[i][0] == cmd && allowed[i][1] == sub;
            assert(jc2_cmd_allowed((u8)cmd, (u8)sub) == want);
            if (!want) assert(!jc2_cmd(b, sizeof b, (u8)cmd, (u8)sub, NULL, 0));
        }
    /* by name: pairing, the host's address and key, flash writes and erases, firmware update, power, the test
       pattern, battery */
    assert(!jc2_cmd_allowed(0x15, 0x03) && !jc2_cmd_allowed(0x15, 0x01) && !jc2_cmd_allowed(0x03, 0x07));
    assert(!jc2_cmd_allowed(0x03, 0x09) && !jc2_cmd_allowed(0x02, 0x05) && !jc2_cmd_allowed(0x02, 0x06));
    assert(!jc2_cmd_allowed(0x02, 0xfd) && !jc2_cmd_allowed(0x0d, 0x04) && !jc2_cmd_allowed(0x06, 0x02));
    assert(!jc2_cmd_allowed(0x07, 0x02) && !jc2_cmd_allowed(0x0f, 0x04) && !jc2_cmd_allowed(0x0b, 0x07));
    for (unsigned h = 0; h < 0x10000; h++) {
        int want = h == 0x0b || h == 0x0f || h == 0x12 || h == 0x14 || h == 0x16 || h == 0x1b || h == 0x1f;
        assert(jc2_handle_writable((u16)h) == want);
    }
    assert(!jc2_handle_writable(0x18) && !jc2_handle_writable(0x2a));
}

/* ---- replies, from the captures */
static void test_replies(void) {
    /* commands.md: the reply to the read of 0x10 bytes at 0x13040, the gyroscope's calibration */
    static const u8 gyro[] = {0x02, 0x01, 0x01, 0x04, 0x10, 0x78, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x40, 0x30,
                              0x01, 0x00, 0x3b, 0xe0, 0xd3, 0x41, 0xc6, 0x60, 0x6a, 0xbc, 0x4d, 0xd7, 0xa2, 0xbb,
                              0x71, 0x1e, 0xdd, 0x37};
    Jc2Reply r;
    float off[3];
    assert(jc2_reply(gyro, sizeof gyro, &r) && r.cmd == 0x02 && r.status == 1 && r.sub == 0x04 && r.size == 24);
    assert(!jc2_reply_flash(&r, 0x13000, 0x10) && !jc2_reply_flash(&r, 0x13040, 0x20));
    const u8* d = jc2_reply_flash(&r, 0x13040, 0x10);
    assert(d == gyro + 16);
    assert(fabsf(snx_ld_lef32(d) - 26.48f) < 0.01f); /* a temperature, the console reads */
    assert(jc2_gyro_cal(d, off));
    assert(fabsf(off[0] + 0.0143f) < 0.0001f && fabsf(off[1] + 0.00497f) < 0.00001f && fabsf(off[2]) < 0.0001f);
    {
        u8 bad[16];
        memcpy(bad, d, 16);
        snx_st_le32(bad + 8, 0x3f800000u); /* 1 rad/s: past the console's 44 degrees/s */
        assert(!jc2_gyro_cal(bad, off) && off[0] == 0 && off[1] == 0 && off[2] == 0);
    }
    assert(!jc2_reply(gyro, 7, &r));
    /* commands.md: 11/03's reply, the IMU's own units: 1 g / 4096 in m/s^2, 1 / 16.4 degrees/s in rad/s per count */
    {
        static const u8 imu[] = {0x01, 0x20, 0x03, 0x00, 0x00, 0x0a, 0xe8, 0x1c, 0x3b, 0x79, 0x7d, 0x8b, 0x3a};
        Jc2Report one = {0};
        float a[3], w[3];
        const float zero[3] = {0};
        one.accel[0] = 1;
        one.gyro[0] = 1;
        jc2_motion(&one, zero, a, w);
        assert(fabsf(-a[0] * 9.80665f / snx_ld_lef32(imu + 5) - 1.0f) < 1e-5f); /* negated: test_motion_frame */
        assert(fabsf(w[0] / snx_ld_lef32(imu + 9) - 1.0f) < 1e-5f);
    }
}

/* ---- the common report */
static void put12(u8* b, u16 x, u16 y) {
    b[0] = (u8)x;
    b[1] = (u8)((x >> 8 & 0x0f) | (y & 0x0f) << 4);
    b[2] = (u8)(y >> 4);
}
static void report(u8 r[JC2_REPORT_SIZE], u32 counter, const u8 buttons[4], u16 lx, u16 ly, u16 rx, u16 ry, u16 mx,
                   u16 my, i16 ax, i16 gz) {
    memset(r, 0, JC2_REPORT_SIZE);
    snx_st_le32(r, counter);
    memcpy(r + 4, buttons, 4);
    put12(r + 0x0a, lx, ly);
    put12(r + 0x0d, rx, ry);
    snx_st_le16(r + 0x10, mx);
    snx_st_le16(r + 0x12, my);
    snx_st_le16(r + 0x1f, 3749);
    snx_st_le32(r + 0x2a, 123456);
    snx_st_le16(r + 0x30, (u16)ax);
    snx_st_le16(r + 0x3a, (u16)gz);
}
static void test_report(void) {
    u8 r[JC2_REPORT_SIZE];
    Jc2Report out;
    /* A and R; Home and C; the left Joy-Con's SL; GL */
    static const u8 b1[4] = {0x08 | 0x40, 0x10 | 0x40, 0x20, 0x02};
    report(r, 7, b1, 0x123, 0xabc, 2048, 2047, 0xfffe, 5, 4096, -164);
    assert(jc2_report(r, sizeof r, JC2_F_JOYCON, &out));
    assert(out.counter == 7 && out.buttons == (1ull << 0 | 1ull << 7 | 1ull << 24));
    assert(out.extra == (JC2_X_HOME | JC2_X_C | JC2_X_GL));
    assert(out.stick[0][0] == 0x123 && out.stick[0][1] == 0xabc && out.stick[1][0] == 2048 && out.stick[1][1] == 2047);
    assert(out.mouse[0] == 0xfffe && out.mouse[1] == 5 && out.battery_mv == 3749 && out.imu_us == 123456);
    assert(out.accel[0] == 4096 && out.gyro[2] == -164);
    {
        float a[3], w[3];
        const float zero[3] = {0};
        jc2_motion(&out, zero, a, w);
        assert(fabsf(a[0] + 1.0f) < 1e-6f && fabsf(w[2] + 10.0f * 3.14159265f / 180.0f) < 1e-5f);
    }
    /* every button bit lands once: the Npad's 0-15 and 24-27, the rest in extra */
    {
        static const u8 all[4] = {0xff, 0x7f, 0xff, 0x03};
        report(r, 0, all, 0, 0, 0, 0, 0, 0, 0, 0);
        assert(jc2_report(r, sizeof r, JC2_F_BUTTONS, &out));
        assert(out.buttons == (0xffffull | 0xfull << 24) && out.extra == 0x1f);
    }
    /* a feature that is off reads as zero; a short report is none */
    report(r, 7, b1, 0x123, 0xabc, 1, 2, 3, 4, 4096, 99);
    assert(jc2_report(r, sizeof r, JC2_F_BUTTONS, &out));
    assert(out.buttons && !out.stick[0][0] && !out.mouse[0] && !out.accel[0] && !out.gyro[2] && !out.imu_us);
    assert(jc2_report(r, sizeof r, 0, &out) && !out.buttons && !out.extra && out.counter == 7);
    assert(!jc2_report(r, JC2_REPORT_SIZE - 1, JC2_F_JOYCON, &out));
    /* the counters' motion, across their wrap */
    assert(jc2_mouse_delta(5, 0xfffe) == 7 && jc2_mouse_delta(0xfffe, 5) == -7);
    assert(jc2_mouse_delta(0x7fff, 0) == 32767 && jc2_mouse_delta(0x8000, 0) == -32768 && !jc2_mouse_delta(9, 9));
}

/* ---- the console's six-axis frame (its hid: every Switch 2 pad, left and right, hands the six-axis filter the IMU's
   acceleration negated and its angular velocity as they are, no axis moved). The samples are a left Joy-Con 2's:
   lying face up it read (-15, 480, 4060) */
static void test_motion_frame(void) {
    Jc2Report flat = {0}, yaw = {0}, roll = {0};
    float a[3], w[3];
    const float zero[3] = {0}, offset[3] = {0.01f, -0.02f, 0.03f};
    flat.accel[0] = -15;
    flat.accel[1] = 480;
    flat.accel[2] = 4060;
    jc2_motion(&flat, zero, a, w);
    /* about -1 g on Z, as gamepad.c's SDL pads give a pad lying face up; X and Y keep their axes, negated too */
    assert(fabsf(a[2] + 4060.0f / 4096.0f) < 1e-6f && a[2] < -0.99f);
    assert(fabsf(a[1] + 480.0f / 4096.0f) < 1e-6f && fabsf(a[0] - 15.0f / 4096.0f) < 1e-6f);
    assert(!w[0] && !w[1] && !w[2]);
    /* the angular velocity keeps axis and sign: a turn to the left lying flat is +Z, the rail side going up -Y */
    yaw.gyro[2] = 1640;
    jc2_motion(&yaw, offset, a, w);
    assert(fabsf(w[2] - (100.0f * 3.14159265f / 180.0f - 0.03f)) < 1e-5f);
    assert(fabsf(w[0] + 0.01f) < 1e-6f && fabsf(w[1] - 0.02f) < 1e-6f);
    roll.gyro[1] = -1640;
    jc2_motion(&roll, zero, a, w);
    assert(w[1] < -1.7f && !w[0] && !w[2]);
    /* one rule for both hands: the function takes no side */
    {
        Jc2Report m = {0};
        m.accel[0] = 4096;
        m.accel[1] = -2048;
        m.gyro[0] = -328;
        m.gyro[1] = 164;
        m.gyro[2] = 82;
        jc2_motion(&m, zero, a, w);
        assert(fabsf(a[0] + 1.0f) < 1e-6f && fabsf(a[1] - 0.5f) < 1e-6f && !a[2]);
        assert(w[0] < 0 && w[1] > 0 && w[2] > 0 && fabsf(w[0] + 2.0f * w[1]) < 1e-6f &&
               fabsf(w[1] - 2.0f * w[2]) < 1e-6f);
    }
}

/* ---- calibration, device info, the advert */
static void test_calibration(void) {
    u8 b[11] = {0xb2, 0xa1};
    Jc2StickCal c;
    put12(b + 2, 2048, 2000);
    put12(b + 5, 1500, 1400);
    put12(b + 8, 1300, 1200);
    assert(jc2_stick_cal(b + 2, &c) && jc2_user_stick_cal(b, &c));
    assert(c.center[0] == 2048 && c.center[1] == 2000 && c.above[0] == 1500 && c.below[1] == 1200);
    assert(jc2_stick_axis(2048, &c, 0) == 0 && jc2_stick_axis(2048 + 1500, &c, 0) == 32767);
    assert(jc2_stick_axis(2048 - 1300, &c, 0) == -32767 && jc2_stick_axis(4095, &c, 0) == 32767);
    assert(jc2_stick_axis(0, &c, 1) == -32767 && jc2_stick_axis(2000 + 700, &c, 1) == 700 * 32767 / 1400);
    b[0] = 0xff; /* no magic: no user calibration */
    assert(!jc2_user_stick_cal(b, &c));
    memset(b, 0xff, sizeof b);
    assert(!jc2_stick_cal(b + 2, &c));
    {
        u8 info[0x40];
        Jc2Info i;
        memset(info, 0xff, sizeof info);
        memcpy(info + 2, "HEJ71001121247", 14); /* memory_layout.md's example serial */
        snx_st_le16(info + 0x12, 0x057e);
        snx_st_le16(info + 0x14, 0x2067);
        info[0x19] = 0x10;
        assert(jc2_info(info, sizeof info, &i) && !strcmp(i.serial, "HEJ71001121247"));
        assert(i.vendor == 0x057e && i.product == JC2_PID_LEFT && i.body[0] == 0x10 && !jc2_info(info, 0x21, &i));
    }
    {
        /* bluetooth_interface.md's adverts, a Pro Controller 2's (69 20), after the company id: sync, reconnect,
           wake */
        u8 ad[24] = {0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0x0f};
        static const u8 host[6] = {0x5f, 0x11, 0x85, 0xeb, 0xf1, 0x48};
        Jc2Advert a;
        assert(jc2_advert(ad, sizeof ad, &a) && a.product == JC2_PID_PRO && a.syncing && !a.wake); /* taken too */
        ad[5] = 0x66;
        assert(jc2_advert(ad, sizeof ad, &a) && a.product == JC2_PID_RIGHT && a.syncing && !a.wake);
        memcpy(ad + 10, host, 6);
        assert(jc2_advert(ad, sizeof ad, &a) && !a.syncing && !a.wake && !memcmp(a.host, host, 6));
        ad[9] = 0x81;
        assert(jc2_advert(ad, sizeof ad, &a) && a.wake);
        ad[5] = 0x67;
        assert(jc2_advert(ad, sizeof ad, &a) && a.product == JC2_PID_LEFT);
        assert(!jc2_advert(ad, 15, &a));
        ad[2] = 0x02;
        assert(!jc2_advert(ad, sizeof ad, &a));
    }
}

/* the launcher's advert check (common/platform/ble_scan.h's snx_ble_joycon2, which Controllers > Bluetooth uses: the
   launcher builds without this tree) agrees with jc2_advert, byte by byte over the fields both read */
static void test_advert_agrees(void) {
    static const u8 base[16] = {0x01, 0x00, 0x03, 0x7e, 0x05, 0x67, 0x20, 0x00, 0x01, 0x00};
    static const u8 values[] = {0x00, 0x01, 0x02, 0x03, 0x05, 0x20, 0x57, 0x66, 0x67, 0x69, 0x7e, 0x80, 0xff};
    int checked = 0;
    for (size_t n = 14; n <= 16; n++)
        for (size_t at = 0; at < 16; at++)
            for (size_t v = 0; v < sizeof values; v++) {
                SnxBleAdvert b;
                SnxBleJoyCon2 j;
                Jc2Advert a;
                int ours, theirs;
                memset(&b, 0, sizeof b);
                memcpy(b.data, base, sizeof base);
                b.data[at] = values[v];
                b.size = n;
                b.company = SNX_BLE_NINTENDO;
                theirs = jc2_advert(b.data, n, &a);
                ours = snx_ble_joycon2(&b, &j);
                assert(ours == theirs);
                if (ours)
                    assert(j.right == (a.product == JC2_PID_RIGHT) && j.pro == (a.product == JC2_PID_PRO) &&
                           j.syncing == a.syncing && j.wake == a.wake);
                checked += ours;
            }
    assert(checked > 0);
}

/* ---- HD rumble */
static void test_rumble(void) {
    u8 s[5], p[JC2_RUMBLE_PACKET];
    /* the neutral bands, 160 and 320 Hz, and the formula's anchor 110 Hz at 333 */
    assert(jc2_rumble_freq(160.0f) == 0x181 && jc2_rumble_freq(320.0f) == 0x1e1 && jc2_rumble_freq(110.0f) == 333);
    assert(jc2_rumble_freq(1.0f) == jc2_rumble_freq(40.0f) && jc2_rumble_freq(9000.0f) == jc2_rumble_freq(3500.0f));
    assert(jc2_rumble_freq(NAN) == 0x181);
    assert(jc2_rumble_amp(1.0f) == 1023 && jc2_rumble_amp(2.0f) == 1023 && !jc2_rumble_amp(-1.0f));
    assert(!jc2_rumble_amp(NAN) && jc2_rumble_amp(0.25f) == 256);
    /* joycon2cpp's "strong" frame, 93 35 36 1C 0D: the four 10-bit fields as the kernel's encoder packs them */
    jc2_rumble_pack(s, 0x193, 0x18d, 0x1c3, 0x034);
    SAME(s, 5, 0x93, 0x35, 0x36, 0x1c, 0x0d);
    /* the packet: 00, the state byte (three samples, the counter), the same sample three times, zeros */
    {
        Jc2Vibration v = {1.0f, 160.0f, 1.0f, 320.0f};
        u8 want[5];
        jc2_rumble_packet(p, &v, 0x13);
        jc2_rumble_pack(want, 0x181, jc2_rumble_amp(0.49f), 0x1e1, jc2_rumble_amp(0.49f)); /* the sum to 0.98 */
        assert(p[0] == 0 && p[1] == 0x73);
        for (int i = 0; i < 3; i++) assert(!memcmp(p + 2 + 5 * i, want, 5));
        for (unsigned i = 17; i < JC2_RUMBLE_PACKET; i++) assert(!p[i]);
    }
    {
        Jc2Vibration v = {0.7f, 100.0f, 0.7f, 200.0f}; /* the sum to 0.98: 0.49 each, under the band's cap */
        jc2_rumble_limit(&v);
        assert(fabsf(v.amp_low - 0.49f) < 1e-6f && fabsf(v.amp_high - 0.49f) < 1e-6f);
        v = (Jc2Vibration){0.8f, 100.0f, 0.1f, 200.0f}; /* strong and weak keep their own strengths */
        jc2_rumble_limit(&v);
        assert(fabsf(v.amp_low - 0.8f) < 1e-6f && fabsf(v.amp_high - 0.1f) < 1e-6f);
        v = (Jc2Vibration){1.0f, 100.0f, 0.0f, 200.0f};
        jc2_rumble_limit(&v);
        assert(fabsf(v.amp_low - JC2_RUMBLE_CAP) < 1e-6f && v.amp_high == 0.0f);
        v = (Jc2Vibration){0.3f, 100.0f, NAN, 200.0f};
        jc2_rumble_limit(&v);
        assert(fabsf(v.amp_low - 0.3f) < 1e-6f && v.amp_high == 0.0f);
    }
}

/* ---- one controller's setup, through the transport */
static size_t reply_bytes(u8* out, u8 cmd, u8 sub, u8 status, const u8* data, size_t n) {
    out[0] = cmd;
    out[1] = status;
    out[2] = 0x01;
    out[3] = sub;
    out[4] = 0x10;
    out[5] = 0x78;
    out[6] = out[7] = 0;
    memcpy(out + 8, data, n);
    return 8 + n;
}
static size_t flash_reply(u8* out, u32 address, u8 n, const u8* bytes) {
    u8 d[8 + 0x40] = {n};
    snx_st_le32(d + 4, address);
    memcpy(d + 8, bytes, n);
    return reply_bytes(out, 0x02, 0x04, 0x01, d, 8u + n);
}
/* the request the transport got last, as a reply would name it */
static const Sent* last(void) { return g_test.n ? &g_test.sent[g_test.n - 1] : NULL; }
static void answer(Jc2Session* s, u64 t, u8 status) {
    const Sent* q = last();
    u8 r[96];
    size_t n;
    assert(q && q->handle == JC2_H_COMMAND);
    if (q->data[0] == 0x02) { /* a read: bytes of its own for each address */
        u32 at = snx_ld_le32(q->data + 12);
        u8 n8 = q->data[8], bytes[0x40];
        memset(bytes, 0xff, sizeof bytes);
        if (at == JC2_FLASH_STICK) {
            put12(bytes + 0x28, 2000, 2100);
            put12(bytes + 0x2b, 1000, 1000);
            put12(bytes + 0x2e, 1000, 1000);
        }
        if (at == JC2_FLASH_GYRO) memset(bytes, 0, sizeof bytes);
        if (at == JC2_FLASH_STICK2) { /* a Pro Controller 2's right stick */
            put12(bytes + 0x28, 1900, 2200);
            put12(bytes + 0x2b, 1000, 1000);
            put12(bytes + 0x2e, 1000, 1000);
        }
        n = flash_reply(r, at, n8, bytes);
    } else if (q->data[0] == 0x10) {
        static const u8 version[12] = {0x01, 0x00, 0x0e, 0x01, 0x0c, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
        n = reply_bytes(r, 0x10, 0x01, status, version, sizeof version); /* commands.md's: 1.0.14, a right one */
    } else {
        static const u8 zero[4] = {0};
        n = reply_bytes(r, q->data[0], q->data[3], status, zero, sizeof zero);
    }
    jc2_session_notify(s, &T, JC2_H_REPLY, r, n, t);
}
static void test_session(void) {
    Jc2Session s;
    u64 t = 5 * S;
    int sends;
    g_test.n = 0;
    jc2_session_start(&s, JC2_SIDE_LEFT);
    /* the reply notifications on, the device info, then the version */
    assert(jc2_session_tick(&s, &T, t) == JC2_SETUP && g_test.n == 1 && g_test.sent[0].cccd == JC2_H_REPLY_CCCD);
    jc2_session_tick(&s, &T, t + 10 * MS);
    SAME(last()->data, last()->n, 0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x30,
         0x01, 0x00);
    answer(&s, t + 11 * MS, 1);
    g_test.n = 1; /* as if the info read went first, for the counts below */
    t += 21 * MS;
    jc2_session_tick(&s, &T, t);
    SAME(last()->data, last()->n, 0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00);
    /* nothing more while the reply is due, also at a clock read before the send; the same request again after 10 s */
    jc2_session_tick(&s, &T, t + 5 * S);
    jc2_session_tick(&s, &T, t - MS);
    assert(g_test.n == 2);
    t += 10 * S + 10 * MS;
    jc2_session_tick(&s, &T, t);
    assert(g_test.n == 3 && same(g_test.sent[2].data, g_test.sent[2].n, g_test.sent[1].data, g_test.sent[1].n));
    /* the reply of another command doesn't end the step */
    {
        u8 r[16];
        static const u8 zero[4] = {0};
        size_t n = reply_bytes(r, 0x0c, 0x02, 1, zero, 4);
        jc2_session_notify(&s, &T, JC2_H_REPLY, r, n, t);
        assert(s.step == 2);
    }
    answer(&s, t + MS, 1);
    assert(s.side == JC2_SIDE_RIGHT && s.version[2] == 14 && s.step == 3);
    /* 10 ms between a reply and the next command; a reply stamped after the tick's clock (two clock reads) keeps
       the pause too, it doesn't wrap */
    jc2_session_tick(&s, &T, t + 5 * MS);
    assert(g_test.n == 3);
    jc2_session_tick(&s, &T, t);
    assert(g_test.n == 3);
    /* every step in its order, each answered */
    sends = g_test.n;
    for (int i = 0; i < 40 && s.state == JC2_SETUP; i++) {
        t += 20 * MS;
        jc2_session_tick(&s, &T, t);
        if (g_test.n > sends && last()->handle == JC2_H_COMMAND) answer(&s, t + MS, 1);
        sends = g_test.n;
    }
    assert(s.state == JC2_READY && s.has_stick && s.stick.center[0] == 2000 && s.stick.center[1] == 2100);
    /* the stream after the version: the reads, then the commands, then the report's notifications */
    {
        static const u8 want[][16] = {
            {0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00},
            {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7e, 0x00, 0x00, 0x80, 0x30, 0x01, 0x00},
            {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7e, 0x00, 0x00, 0x40, 0xc0, 0x1f, 0x00},
            {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x10, 0x7e, 0x00, 0x00, 0x40, 0x30, 0x01, 0x00},
            {0x11, 0x91, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00},
            {0x0a, 0x91, 0x01, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0x59, 0x09, 0x00, 0x00, 0xff, 0xff, 0xff},
            {0x11, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00},
            {0x13, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00},
            {0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00},
            {0x0a, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}};
        static const size_t sizes[] = {12, 16, 16, 16, 8, 28, 8, 8, 12, 12};
        assert(g_test.n == 3 + 10 + 1);
        for (int i = 0; i < 10; i++) {
            const Sent* q = &g_test.sent[3 + i];
            assert(q->handle == JC2_H_COMMAND && q->n == sizes[i] && !memcmp(q->data, want[i], q->n < 16 ? q->n : 16));
        }
        /* the actuator's parameters whole, as the console's capture has them */
        SAME(g_test.sent[8].data, g_test.sent[8].n, 0x0a, 0x91, 0x01, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0x59, 0x09, 0x00,
             0x00, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
        assert(g_test.sent[13].handle == 0xffff && g_test.sent[13].cccd == JC2_H_COMMON_CCCD);
    }
    /* reports: a right Joy-Con's stick is the report's right one, calibrated; the first report's counters only set
       where the motion starts */
    {
        u8 r[JC2_REPORT_SIZE];
        static const u8 a[4] = {0x08, 0, 0, 0};
        i32 m[2];
        report(r, 1, a, 0, 0, 3000, 1100, 100, 200, 4096, 0);
        jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
        jc2_session_mouse(&s, m);
        assert(!m[0] && !m[1] && s.pad.buttons == 1 && s.pad.stick[0] == 32767 && s.pad.stick[1] == -32767);
        assert(fabsf(s.pad.accel[0] + 1.0f) < 1e-6f && s.pad.reports == 1);
        report(r, 2, a, 0, 0, 2000, 2100, 90, 0xfff0, 0, 0);
        jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
        report(r, 3, a, 0, 0, 2000, 2100, 95, 0xfff8, 0, 0);
        jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
        jc2_session_mouse(&s, m);
        assert(m[0] == -5 && m[1] == -200 - 8 && !s.pad.stick[0] && !s.pad.stick[1]);
        /* the surface byte as measured: on a table about 10, a palm about 115, a hand about 200, lifted 0xff */
        {
            static const u8 at[] = {10, 115, 200, 0xff, 0x35};
            for (int k = 0; k < 5; k++) {
                r[0x14] = at[k];
                jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
                assert(s.pad.on_surface == (k == 0));
            }
            r[0x14] = 0;
            jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
            jc2_session_mouse(&s, m);
        }
        jc2_session_mouse(&s, m);
        assert(!m[0] && !m[1]);
    }
    /* the player lights once, then the vibration: again only after 15 ms, one stop when it ends */
    sends = g_test.n;
    jc2_session_led(&s, 0x03);
    t += S;
    jc2_session_tick(&s, &T, t);
    assert(g_test.n == sends + 1);
    SAME(last()->data, last()->n, 0x09, 0x91, 0x01, 0x07, 0x00, 0x08, 0x00, 0x00, 0x03, 0, 0, 0, 0, 0, 0, 0);
    answer(&s, t + MS, 1);
    jc2_session_tick(&s, &T, t + 20 * MS);
    assert(g_test.n == sends + 1);
    {
        Jc2Vibration v = {0.5f, 160.0f, 0.0f, 320.0f}, stop = {0.0f, 160.0f, 0.0f, 320.0f};
        jc2_session_rumble(&s, &v);
        jc2_session_tick(&s, &T, t + 30 * MS);
        assert(g_test.n == sends + 2 && last()->handle == JC2_H_VIBRATION && last()->n == JC2_RUMBLE_PACKET);
        jc2_session_tick(&s, &T, t + 40 * MS);
        assert(g_test.n == sends + 2);
        /* past 15 ms, but no report since the write: the link may not have carried it yet, so none goes */
        jc2_session_tick(&s, &T, t + 45 * MS);
        assert(g_test.n == sends + 2);
        {
            u8 r[JC2_REPORT_SIZE];
            static const u8 none[4] = {0, 0, 0, 0};
            report(r, 4, none, 0, 0, 2000, 2100, 95, 0xfff8, 0, 0);
            jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t + 45 * MS);
        }
        jc2_session_tick(&s, &T, t + 45 * MS);
        assert(g_test.n == sends + 3 && (last()->data[1] & 0x0f) == 1);
        jc2_session_rumble(&s, &stop);
        jc2_session_tick(&s, &T, t + 46 * MS);
        const u8* z = last()->data + 2; /* the first sample: both amplitudes 0 */
        assert(g_test.n == sends + 4 && !(z[1] >> 2) && !(z[2] & 0x0f) && !(z[3] >> 6) && !z[4]);
        jc2_session_tick(&s, &T, t + 100 * MS);
        assert(g_test.n == sends + 4);
    }
}
/* a Pro Controller 2 (its advert's product 2069): its own setup (the console's feature set 0x27, the second stick's
   calibration read, no mouse sensor start and no Joy-Con actuator parameters), both sticks from the common report,
   each by its calibration, and two actuator blocks in one vibration write, the right one's after the left one's */
static void test_session_pro(void) {
    Jc2Session s;
    Jc2Advert a;
    u64 t = 5 * S;
    int sends;
    static const u8 advert[16] = {0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01, 0x80};
    assert(jc2_advert(advert, sizeof advert, &a) && a.product == JC2_PID_PRO && a.syncing && a.wake);
    g_test.n = 0;
    jc2_session_start(&s, JC2_SIDE_PRO);
    sends = g_test.n;
    for (int i = 0; i < 60 && s.state == JC2_SETUP; i++) {
        t += 20 * MS;
        jc2_session_tick(&s, &T, t);
        if (g_test.n > sends && last()->handle == JC2_H_COMMAND) answer(&s, t + MS, 1);
        sends = g_test.n;
    }
    assert(s.state == JC2_READY && s.side == JC2_SIDE_PRO); /* a version saying a right Joy-Con keeps it a Pro */
    assert(s.stick.center[0] == 2000 && s.stick2.center[0] == 1900 && s.stick2.center[1] == 2200);
    {
        int features = 0, stick2 = 0;
        for (int i = 0; i < g_test.n; i++) {
            const Sent* q = &g_test.sent[i];
            if (q->handle != JC2_H_COMMAND) continue;
            assert(q->data[0] != 0x13 && !(q->data[0] == 0x0a && q->data[3] == 0x08)); /* no mouse, no Joy-Con params */
            if (q->data[0] == 0x0c) features += q->data[8] == JC2_F_PRO;
            if (q->data[0] == 0x02 && snx_ld_le32(q->data + 12) == JC2_FLASH_STICK2) stick2++;
        }
        assert(features == 2 && stick2 == 1);
    }
    {
        u8 r[JC2_REPORT_SIZE];
        static const u8 a_and_zl[4] = {0x08, 0, 0x80, 0};
        report(r, 1, a_and_zl, 3000, 1100, 1900, 3200, 0, 0, 0, 0);
        jc2_session_notify(&s, &T, JC2_H_COMMON, r, sizeof r, t);
        assert(s.pad.buttons == (1ull | 1ull << 8) && s.pad.stick[0] == 32767 && s.pad.stick[1] == -32767);
        assert(s.pad.rstick[0] == 0 && s.pad.rstick[1] == 32767 && !s.pad.on_surface);
    }
    {
        Jc2Vibration left = {0.0f, 160.0f, 0.0f, 320.0f}, right = {0.5f, 160.0f, 0.0f, 320.0f};
        u8 p[JC2_RUMBLE_PACKET];
        jc2_session_rumble(&s, &left);
        jc2_session_rumble2(&s, &right);
        sends = g_test.n;
        jc2_session_tick(&s, &T, t + S);
        assert(g_test.n == sends + 1 && last()->handle == JC2_H_VIBRATION && last()->n == JC2_RUMBLE_PACKET);
        jc2_rumble_packet_pro(p, &left, &right, 0);
        assert(!memcmp(last()->data, p, sizeof p) && (p[1] & 0xf0) == 0x70 && (p[17] & 0xf0) == 0x70);
        /* the left one's first sample's low amplitude 0, the right one's not (bits 10 to 19 of each sample) */
        assert(!(p[3] >> 2) && !(p[4] & 0x0f) && ((p[19] >> 2) | (p[20] & 0x0f)));
        for (int i = 33; i < (int)JC2_RUMBLE_PACKET; i++) assert(!p[i]);
    }
}
/* a version that never comes ends the setup after 5 sends; a read or a refusal of an optional step is skipped */
static void test_session_failures(void) {
    Jc2Session s;
    u64 t = S;
    g_test.n = 0;
    jc2_session_start(&s, JC2_SIDE_LEFT);
    jc2_session_tick(&s, &T, t);
    jc2_session_tick(&s, &T, t + 20 * MS);
    answer(&s, t + 21 * MS, 1); /* the device info */
    assert(s.step == 2);
    for (int i = 0; i < 8; i++) jc2_session_tick(&s, &T, t += 11 * S);
    assert(s.state == JC2_FAILED && g_test.n == 2 + JC2_SENDS);
    /* the transport refusing the CCCD write 5 times */
    g_test.n = 0;
    g_test.fail = 1;
    jc2_session_start(&s, JC2_SIDE_LEFT);
    for (int i = 0; i < 5; i++) jc2_session_tick(&s, &T, t + (u64)i * S);
    assert(s.state == JC2_FAILED && !g_test.n);
    g_test.fail = 0;
    /* the device info read unanswered 5 times: skipped */
    g_test.n = 0;
    jc2_session_start(&s, JC2_SIDE_LEFT);
    jc2_session_tick(&s, &T, t);
    for (int i = 0; i < 6 && s.step == 1; i++) jc2_session_tick(&s, &T, t += 11 * S);
    assert(s.state == JC2_SETUP && s.step == 2 && g_test.n == 1 + JC2_SENDS);
    /* step 2 is the version: on to the stick's read (step 4), where a late reply to another address is not its own */
    while (s.state == JC2_SETUP && s.step < 4) {
        int had = g_test.n;
        jc2_session_tick(&s, &T, t += 20 * MS);
        if (g_test.n > had) answer(&s, t + MS, 1);
    }
    jc2_session_tick(&s, &T, t += 20 * MS);
    assert(s.step == 4 && snx_ld_le32(last()->data + 12) == JC2_FLASH_STICK);
    {
        u8 r[96], bytes[0x40] = {0};
        size_t n = flash_reply(r, JC2_FLASH_INFO, 0x40, bytes); /* the device info's, late */
        jc2_session_notify(&s, &T, JC2_H_REPLY, r, n, t + MS);
        assert(s.step == 4 && s.state == JC2_SETUP && s.sent_ns);
    }
    answer(&s, t + 2 * MS, 1);
    assert(s.step == 5);
    /* the actuator's parameters (step 8) refused: skipped; the feature set (step 3) refused: the setup stops */
    while (s.state == JC2_SETUP && s.step < 8) {
        int had = g_test.n;
        jc2_session_tick(&s, &T, t += 20 * MS);
        if (g_test.n > had) answer(&s, t + MS, 1);
    }
    assert(s.step == 8);
    jc2_session_tick(&s, &T, t += 20 * MS);
    assert(last()->data[0] == JC2_CMD_VIBRATION && last()->data[3] == 0x08);
    answer(&s, t + MS, 0x00);
    assert(s.step == 9 && s.state == JC2_SETUP);
    s.step = 3;
    s.sent_ns = 0;
    jc2_session_tick(&s, &T, t += 20 * MS);
    assert(last()->data[0] == JC2_CMD_FEATURES && last()->data[3] == 0x02);
    answer(&s, t + MS, 0x04);
    assert(s.state == JC2_FAILED);
}

/* ---- ATT's PDUs: writes only to the handles allowed, a CCCD only by a write request */
static void test_att(void) {
    u8 b[80], cmd[16];
    Jc2Att a;
    size_t n = jc2_att_mtu_req(b, sizeof b, JC2_ATT_MTU);
    SAME(b, n, 0x02, 0x00, 0x02);
    n = jc2_cmd(cmd, sizeof cmd, JC2_CMD_VERSION, 0x01, NULL, 0);
    n = jc2_att_write_cmd(b, sizeof b, JC2_H_COMMAND, cmd, n);
    SAME(b, n, 0x52, 0x14, 0x00, 0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00);
    assert(jc2_att_write_cmd(b, sizeof b, JC2_H_VIBRATION, cmd, 8) && jc2_att_write_cmd(b, sizeof b, 0x16, cmd, 8));
    assert(!jc2_att_write_cmd(b, sizeof b, 0x18, cmd, 8) && !jc2_att_write_cmd(b, sizeof b, 0x2a, cmd, 8));
    assert(!jc2_att_write_cmd(b, sizeof b, JC2_H_REPLY_CCCD, cmd, 2) && !jc2_att_write_cmd(b, 10, 0x14, cmd, 8));
    n = jc2_att_write_cccd(b, sizeof b, JC2_H_REPLY_CCCD, 1);
    SAME(b, n, 0x12, 0x1b, 0x00, 0x01, 0x00);
    n = jc2_att_write_cccd(b, sizeof b, JC2_H_COMMON_CCCD, 0);
    SAME(b, n, 0x12, 0x0b, 0x00, 0x00, 0x00);
    assert(!jc2_att_write_cccd(b, sizeof b, JC2_H_COMMAND, 1) && !jc2_att_write_cccd(b, sizeof b, 0x2a, 1));
    assert(!jc2_att_write_cccd(b, sizeof b, 0x23, 1) && !jc2_att_write_cccd(b, sizeof b, 0x05, 1));
    /* what the controller sends */
    memset(b, 0, sizeof b);
    b[0] = JC2_ATT_NOTIFY;
    snx_st_le16(b + 1, JC2_H_COMMON);
    assert(jc2_att_parse(b, 3 + JC2_REPORT_SIZE, &a) && a.op == JC2_ATT_NOTIFY && a.handle == JC2_H_COMMON);
    assert(a.value == b + 3 && a.n == JC2_REPORT_SIZE && !jc2_att_answer(&a, cmd, sizeof cmd, 512));
    {
        static const u8 rsp[] = {0x03, 0xf7, 0x00}, err[] = {0x01, 0x12, 0x1b, 0x00, 0x03};
        assert(jc2_att_parse(rsp, sizeof rsp, &a) && a.op == JC2_ATT_MTU_RSP && a.mtu == 247);
        assert(jc2_att_parse(err, sizeof err, &a) && a.request == 0x12 && a.handle == 0x1b && a.error == 3);
        assert(!jc2_att_parse(err, 4, &a) && !jc2_att_parse(rsp, 2, &a) && !jc2_att_parse(rsp, 0, &a));
    }
    {
        static const u8 mtu[] = {0x02, 0xf7, 0x00}, ind[] = {0x1d, 0x26, 0x00, 0x01}, read[] = {0x0a, 0x03, 0x00};
        assert(jc2_att_parse(mtu, sizeof mtu, &a));
        n = jc2_att_answer(&a, b, sizeof b, JC2_ATT_MTU);
        SAME(b, n, 0x03, 0x00, 0x02);
        assert(jc2_att_parse(ind, sizeof ind, &a) && a.handle == 0x26 && a.n == 1);
        n = jc2_att_answer(&a, b, sizeof b, JC2_ATT_MTU);
        SAME(b, n, 0x1e);
        assert(jc2_att_parse(read, sizeof read, &a));
        n = jc2_att_answer(&a, b, sizeof b, JC2_ATT_MTU);
        SAME(b, n, 0x01, 0x0a, 0x00, 0x00, 0x06);
    }
}

int main(void) {
    test_requests();
    test_refusals();
    test_replies();
    test_report();
    test_motion_frame();
    test_calibration();
    test_advert_agrees();
    test_rumble();
    test_session();
    test_session_failures();
    test_session_pro();
    test_att();
    printf("joycon2: ok\n");
    return 0;
}
