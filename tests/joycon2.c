/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2's protocol (runtime/platform/joycon2_proto.c) on bytes made here, no Bluetooth: the requests against
   the bytes that public captures show (ndeadly's switch2_controller_research, commands.md and bluetooth_interface.md),
   the commands and handles the host must never send or write, the replies of those captures, the common report's
   fields and the features that gate them, the calibration and the HD rumble's codes against a public frame
   (joycon2cpp's) */
#include "nx/runtime/platform/joycon2_proto.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
        assert(!jc2_advert(ad, sizeof ad, &a)); /* not a Joy-Con 2 */
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

int main(void) {
    test_requests();
    test_refusals();
    test_replies();
    test_report();
    test_motion_frame();
    test_calibration();
    test_rumble();
    printf("joycon2: ok\n");
    return 0;
}
