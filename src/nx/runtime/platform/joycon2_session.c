/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One Joy-Con 2's setup and input (joycon2_session.h). The setup is the console's own order cut to what a host that
   doesn't pair needs (ndeadly's bluetooth_interface.md, "Console initialisation sequence"): the reply notifications
   on, the device info, the version (which says left or right), the feature set with the mouse sensor, the stick and
   gyroscope calibration, the IMU's ranges, the actuator's parameters, the IMU and mouse sensor started, the features
   on, the connection's double click, then the common report's notifications. Left out: 07/01 and 16/01 (their meaning
   is unknown), the 0x13100 and 0x13060 reads (nothing uses them yet), and the pairing (0x15) and the host's address
   and key (03/07, 03/09) that the console sends between them */
#include "nx/runtime/platform/joycon2_session.h"
#include <string.h>

enum { STEP_NOTIFY, STEP_CMD, STEP_READ };
typedef struct {
    u8 kind;
    u8 cmd, sub, n; /* STEP_CMD: the command and its payload's size; STEP_READ: the size read */
    u32 at;         /* STEP_READ: the flash address; STEP_NOTIFY: the CCCD */
    int optional;   /* a refusal or no reply skips it */
    u8 payload[20];
} Step;
static const Step STEPS[] = {
    {STEP_NOTIFY, 0, 0, 0, JC2_H_REPLY_CCCD, 0, {0}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_INFO, 1, {0}},
    {STEP_CMD, JC2_CMD_VERSION, 0x01, 0, 0, 0, {0}},
    {STEP_CMD, JC2_CMD_FEATURES, 0x02, 4, 0, 0, {JC2_F_JOYCON}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_STICK, 1, {0}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_USER_STICK, 1, {0}},
    {STEP_READ, 0, 0, 0x10, JC2_FLASH_GYRO, 1, {0}},
    {STEP_CMD, JC2_CMD_IMU, 0x03, 0, 0, 1, {0}},
    /* the console's 20 bytes for a Joy-Con 2's actuator (bluetooth_interface.md); their fields are unknown */
    {STEP_CMD, JC2_CMD_VIBRATION, 0x08, 20, 0, 1,
     {0x01, 0x59, 0x09, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46, 0x00}},
    {STEP_CMD, JC2_CMD_IMU, 0x01, 0, 0, 1, {0}},
    {STEP_CMD, JC2_CMD_MOUSE, 0x01, 0, 0, 1, {0}},
    {STEP_CMD, JC2_CMD_FEATURES, 0x04, 4, 0, 0, {JC2_F_JOYCON}},
    {STEP_CMD, JC2_CMD_VIBRATION, 0x02, 4, 0, 1, {3}}, /* the built-in double click a console plays on connect */
    {STEP_NOTIFY, 0, 0, 0, JC2_H_COMMON_CCCD, 0, {0}},
};
#define STEPS_N ((int)(sizeof STEPS / sizeof *STEPS))
/* a Pro Controller 2's: its feature set (the console's), both sticks' calibration, no mouse sensor, and none of the
   Joy-Con 2's actuator parameters (the Pro Controller 2's are not known) */
static const Step STEPS_PRO[] = {
    {STEP_NOTIFY, 0, 0, 0, JC2_H_REPLY_CCCD, 0, {0}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_INFO, 1, {0}},
    {STEP_CMD, JC2_CMD_VERSION, 0x01, 0, 0, 0, {0}},
    {STEP_CMD, JC2_CMD_FEATURES, 0x02, 4, 0, 0, {JC2_F_PRO}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_STICK, 1, {0}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_STICK2, 1, {0}},
    {STEP_READ, 0, 0, 0x40, JC2_FLASH_USER_STICK, 1, {0}},
    {STEP_READ, 0, 0, 0x10, JC2_FLASH_GYRO, 1, {0}},
    {STEP_CMD, JC2_CMD_IMU, 0x03, 0, 0, 1, {0}},
    {STEP_CMD, JC2_CMD_IMU, 0x01, 0, 0, 1, {0}},
    {STEP_CMD, JC2_CMD_FEATURES, 0x04, 4, 0, 0, {JC2_F_PRO}},
    {STEP_CMD, JC2_CMD_VIBRATION, 0x02, 4, 0, 1, {3}},
    {STEP_NOTIFY, 0, 0, 0, JC2_H_COMMON_CCCD, 0, {0}},
};
#define STEPS_PRO_N ((int)(sizeof STEPS_PRO / sizeof *STEPS_PRO))
/* the session's setup steps, and how many */
static const Step* steps_of(const Jc2Session* s) { return s->side == JC2_SIDE_PRO ? STEPS_PRO : STEPS; }
static int steps_n(const Jc2Session* s) { return s->side == JC2_SIDE_PRO ? STEPS_PRO_N : STEPS_N; }
/* a stick with no calibration in its flash: centred, a plain travel (the public notes' "about 2048") */
static const Jc2StickCal STICK_PLAIN = {{2048, 2048}, {1600, 1600}, {1600, 1600}};

void jc2_session_start(Jc2Session* s, int side) {
    *s = (Jc2Session){0};
    s->side = side;
    s->stick = STICK_PLAIN;
    s->stick2 = STICK_PLAIN;
}

/* the step's command (its request into buf); 0 when the builder refuses it */
static size_t step_request(const Step* st, u8* buf, size_t size) {
    if (st->kind == STEP_READ) return jc2_cmd_flash_read(buf, size, st->at, st->n);
    return jc2_cmd(buf, size, st->cmd, st->sub, st->payload, st->n);
}
static void step_done(Jc2Session* s, u64 now_ns) {
    s->step++;
    s->sends = 0;
    s->sent_ns = 0;
    s->reply_ns = now_ns;
    if (s->step >= steps_n(s)) s->state = JC2_READY;
}
/* the step failed: an optional one is skipped, else the setup stops */
static void step_failed(Jc2Session* s, u64 now_ns) {
    if (steps_of(s)[s->step].optional)
        step_done(s, now_ns);
    else
        s->state = JC2_FAILED;
}
static void step_send(Jc2Session* s, const Jc2Transport* t, u64 now_ns) {
    const Step* st = &steps_of(s)[s->step];
    u8 buf[JC2_CMD_HEADER + JC2_CMD_PAYLOAD];
    if (st->kind == STEP_NOTIFY) { /* its write has a response of its own: done when it went */
        if (t->notify(t->link, (u16)st->at, 1)) {
            if (++s->sends >= JC2_SENDS) s->state = JC2_FAILED;
            return;
        }
        step_done(s, now_ns);
        return;
    }
    size_t n = step_request(st, buf, sizeof buf);
    s->sends++;
    if (!n || t->write(t->link, JC2_H_COMMAND, buf, n)) {
        if (!n || s->sends >= JC2_SENDS) step_failed(s, now_ns);
        return;
    }
    s->sent_ns = now_ns ? now_ns : 1;
}

/* after the setup: the player lights when they changed, one command waiting for its reply at a time */
static void ready_tick(Jc2Session* s, const Jc2Transport* t, u64 now_ns) {
    if (s->sent_ns && now_ns < s->sent_ns + JC2_REPLY_TIMEOUT_NS) return;
    s->sent_ns = 0;
    if (s->led && s->led != s->led_sent && now_ns >= s->reply_ns + JC2_GUARD_NS) {
        const u8 p[8] = {s->led}; /* the console's 8 bytes, the pattern first */
        u8 buf[JC2_CMD_HEADER + sizeof p];
        size_t n = jc2_cmd(buf, sizeof buf, JC2_CMD_LED, 0x07, p, sizeof p);
        if (n && !t->write(t->link, JC2_H_COMMAND, buf, n)) {
            s->led_sent = s->led;
            s->sent_ns = now_ns ? now_ns : 1;
        }
    }
}
/* the vibration: written again every JC2_RUMBLE_GAP_NS while it plays, a stop once when it ends */
static void rumble_tick(Jc2Session* s, const Jc2Transport* t, u64 now_ns) {
    int on = s->rumble.amp_low > 0.0f || s->rumble.amp_high > 0.0f ||
             (s->side == JC2_SIDE_PRO && (s->rumble2.amp_low > 0.0f || s->rumble2.amp_high > 0.0f));
    if (!on && !s->rumble_on) return;
    /* while it plays: at most one write a report, the newest value. A report comes at each connection event, so the
       writes never outrun what the link carries (at a 30-50 ms interval they queued and played seconds late) */
    if (on && s->rumble_on && (now_ns < s->rumble_ns + JC2_RUMBLE_GAP_NS || s->pad.reports == s->rumble_reports)) return;
    u8 packet[JC2_RUMBLE_PACKET];
    if (s->side == JC2_SIDE_PRO)
        jc2_rumble_packet_pro(packet, &s->rumble, &s->rumble2, s->rumble_counter);
    else
        jc2_rumble_packet(packet, &s->rumble, s->rumble_counter);
    if (t->write(t->link, JC2_H_VIBRATION, packet, sizeof packet)) return;
    s->rumble_counter++;
    s->rumble_ns = now_ns;
    s->rumble_reports = s->pad.reports;
    s->rumble_on = on;
}
int jc2_session_tick(Jc2Session* s, const Jc2Transport* t, u64 now_ns) {
    if (s->state == JC2_READY) {
        ready_tick(s, t, now_ns);
        rumble_tick(s, t, now_ns);
        return s->state;
    }
    if (s->state != JC2_SETUP) return s->state;
    if (s->sent_ns) { /* waiting for the reply */
        if (now_ns < s->sent_ns + JC2_REPLY_TIMEOUT_NS) return s->state;
        s->sent_ns = 0;
        if (s->sends >= JC2_SENDS) {
            step_failed(s, now_ns);
            return s->state;
        }
    }
    if (s->reply_ns && now_ns < s->reply_ns + JC2_GUARD_NS) return s->state;
    step_send(s, t, now_ns);
    return s->state;
}

/* a flash read's bytes, where the setup keeps them */
static void step_read(Jc2Session* s, u32 at, const u8* d) {
    Jc2StickCal c;
    switch (at) {
        case JC2_FLASH_INFO: jc2_info(d, 0x40, &s->info); break;
        case JC2_FLASH_STICK:
            if (jc2_stick_cal(d + 0x28, &c)) {
                s->stick = c;
                s->has_stick = 1;
            }
            break;
        case JC2_FLASH_STICK2:
            if (jc2_stick_cal(d + 0x28, &c)) s->stick2 = c;
            break;
        case JC2_FLASH_USER_STICK: /* the user's calibration, made in the console's settings, comes first */
            if (jc2_user_stick_cal(d, &c)) {
                s->stick = c;
                s->has_stick = 1;
            }
            if (s->side == JC2_SIDE_PRO && jc2_user_stick_cal(d + 0x20, &c)) s->stick2 = c;
            break;
        case JC2_FLASH_GYRO: jc2_gyro_cal(d, s->gyro_offset); break;
        default: break;
    }
}
static void on_reply(Jc2Session* s, const u8* p, size_t n, u64 now_ns) {
    Jc2Reply r;
    if (!s->sent_ns || !jc2_reply(p, n, &r)) return;
    if (s->state == JC2_READY) { /* a later command's */
        s->sent_ns = 0;
        s->reply_ns = now_ns;
        return;
    }
    const Step* st = &steps_of(s)[s->step];
    if (st->kind == STEP_READ) {
        if (r.cmd != JC2_CMD_FLASH || r.sub != 0x04) return; /* not this step's */
        if (r.status != 1) {
            step_failed(s, now_ns);
            return;
        }
        const u8* d = jc2_reply_flash(&r, st->at, st->n);
        if (!d) return; /* a late reply to another read (one sent again after its timeout): not this one's */
        step_read(s, st->at, d);
    } else {
        if (r.cmd != st->cmd || r.sub != st->sub) return;
        if (r.status != 1) {
            step_failed(s, now_ns);
            return;
        }
        /* the version: three numbers, then 0 for a left Joy-Con 2, 1 for a right one */
        if (st->cmd == JC2_CMD_VERSION && r.size >= 4) {
            memcpy(s->version, r.data, 3);
            if (s->side != JC2_SIDE_PRO && r.data[3] <= 1) s->side = r.data[3]; /* a Pro Controller 2 stays one */
        }
    }
    if (s->state == JC2_SETUP) step_done(s, now_ns);
}
static void on_report(Jc2Session* s, const u8* p, size_t n, u64 now_ns) {
    Jc2Report r;
    int pro = s->side == JC2_SIDE_PRO;
    if (!jc2_report(p, n, pro ? JC2_F_PRO : JC2_F_JOYCON, &r)) return;
    const u16* raw = r.stick[s->side == JC2_SIDE_RIGHT];
    s->pad.buttons = r.buttons;
    s->pad.extra = r.extra;
    s->pad.stick[0] = jc2_stick_axis(raw[0], &s->stick, 0);
    s->pad.stick[1] = jc2_stick_axis(raw[1], &s->stick, 1);
    if (pro) { /* both sticks, the right one by its own calibration */
        s->pad.rstick[0] = jc2_stick_axis(r.stick[1][0], &s->stick2, 0);
        s->pad.rstick[1] = jc2_stick_axis(r.stick[1][1], &s->stick2, 1);
    }
    jc2_motion(&r, s->gyro_offset, s->pad.accel, s->pad.gyro);
    s->pad.battery_mv = r.battery_mv;
    (void)now_ns;
    s->pad.on_surface = r.mouse_surface < JC2_SURFACE_NEAR;
    s->pad.reports++;
    /* the counters' motion: none from the first report, which only sets where they start */
    for (int i = 0; s->mouse_seen && i < 2; i++) {
        i64 sum = (i64)s->mouse[i] + jc2_mouse_delta(r.mouse[i], s->mouse_last[i]);
        s->mouse[i] = (i32)(sum > 0x7fffffff ? 0x7fffffff : sum < -0x7fffffff ? -0x7fffffff : sum);
    }
    s->mouse_last[0] = r.mouse[0];
    s->mouse_last[1] = r.mouse[1];
    s->mouse_seen = 1;
}
void jc2_session_notify(Jc2Session* s, const Jc2Transport* t, u16 handle, const u8* p, size_t n, u64 now_ns) {
    (void)t;
    if (handle == JC2_H_REPLY)
        on_reply(s, p, n, now_ns);
    else if (handle == JC2_H_COMMON)
        on_report(s, p, n, now_ns);
}
void jc2_session_led(Jc2Session* s, u8 pattern) { s->led = pattern & 0x0f; }
void jc2_session_rumble(Jc2Session* s, const Jc2Vibration* v) { s->rumble = *v; }
void jc2_session_rumble2(Jc2Session* s, const Jc2Vibration* right) { s->rumble2 = *right; }
void jc2_session_mouse(Jc2Session* s, i32 out[2]) {
    out[0] = s->mouse[0];
    out[1] = s->mouse[1];
    s->mouse[0] = s->mouse[1] = 0;
}
