/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Joy-Con 2's Bluetooth LE protocol as bytes alone, no OS and no SDL: the advert, the fixed GATT handles, the
   commands (an 8-byte header, then the payload), their replies, the common input report, the calibration in the
   controller's flash, and the HD rumble block. Facts from public reverse engineering: ndeadly's
   switch2_controller_research (bluetooth_interface.md, commands.md, hid_reports.md, memory_layout.md), the mainline
   kernel's hid-nintendo.h, linux-switch2's PROTOCOL.md; nothing is ported from them. Tried on a left and a right
   Joy-Con 2.

   The host only sends what jc2_cmd_allowed lets through: reads, feature and LED switches, the IMU and mouse sensor
   starts, the vibration presets. Pairing (0x15), the transport commands that store a host (0x03), flash writes and
   erases, firmware updates (0x0D) and power commands cannot be built, so a connection never changes what the
   controller keeps: it stays paired with its console */
#ifndef SNX_JOYCON2_PROTO_H
#define SNX_JOYCON2_PROTO_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* the advert's manufacturer data: the console maker's company id, then 01 00 03, the vendor and product ids */
#define JC2_COMPANY   0x0553u
#define JC2_VENDOR    0x057eu
#define JC2_PID_RIGHT 0x2066u
#define JC2_PID_LEFT  0x2067u
#define JC2_PID_PRO   0x2069u /* the Pro Controller 2 */

/* the main service's attribute handles, the same on every Joy-Con 2 (the console doesn't discover them) */
#define JC2_H_COMMON          0x000au /* the common input report, notify */
#define JC2_H_COMMON_CCCD     0x000bu
#define JC2_H_NATIVE          0x000eu /* the left or right one's own report, notify */
#define JC2_H_NATIVE_CCCD     0x000fu
#define JC2_H_VIBRATION       0x0012u /* vibration alone, write without response */
#define JC2_H_COMMAND         0x0014u /* a command alone, write without response */
#define JC2_H_VIBRATION_CMD   0x0016u /* vibration, then a command */
#define JC2_H_REPLY           0x001au /* the replies to JC2_H_COMMAND, notify */
#define JC2_H_REPLY_CCCD      0x001bu
#define JC2_H_REPLY2          0x001eu /* the replies to JC2_H_VIBRATION_CMD */
#define JC2_H_REPLY2_CCCD     0x001fu
/* whether the host writes a handle: the ones above that take writes. Never 0x18 (the large commands of a firmware
   update) nor 0x2a, whose write is said to crash the controller */
int jc2_handle_writable(u16 handle);

/* the commands the host sends, and their subcommands */
#define JC2_CMD_FLASH      0x02u /* 04: read */
#define JC2_CMD_LED        0x09u /* 07: the player lights' pattern, 08: flash them */
#define JC2_CMD_VIBRATION  0x0au /* 02: play a built-in pattern, 08: the actuator's parameters */
#define JC2_CMD_FEATURES   0x0cu /* 02: the set, 04: enable, 05: disable */
#define JC2_CMD_VERSION    0x10u /* 01 */
#define JC2_CMD_IMU        0x11u /* 01: start, 03: its ranges */
#define JC2_CMD_MOUSE      0x13u /* 01: the optical sensor's state */
/* the feature bits of JC2_CMD_FEATURES */
#define JC2_F_BUTTONS      0x01u
#define JC2_F_STICKS       0x02u
#define JC2_F_IMU          0x04u
#define JC2_F_MOUSE        0x10u
#define JC2_F_RUMBLE       0x20u
#define JC2_F_JOYCON       0x37u /* the console's set for a Joy-Con 2 */
#define JC2_F_PRO          0x27u /* and for the Pro Controller 2: buttons, sticks, IMU, rumble (public notes saw 0x2f) */
#define JC2_CMD_HEADER     8u
#define JC2_CMD_PAYLOAD    32u /* the largest the host sends */
#define JC2_FLASH_READ_MAX 0x40u
/* whether the host may send cmd/sub */
int jc2_cmd_allowed(u8 cmd, u8 sub);
/* the request for cmd/sub with payload's n bytes into out: `cmd 91 01 sub len(16-bit, big-endian) 00 00 payload`.
   Its size, 0 when the command isn't allowed or out has no room */
size_t jc2_cmd(u8* out, size_t size, u8 cmd, u8 sub, const u8* payload, size_t n);
/* a flash read of n bytes at address: n up to JC2_FLASH_READ_MAX, within one 0x1000 page; 0 otherwise */
size_t jc2_cmd_flash_read(u8* out, size_t size, u32 address, u8 n);
/* a JC2_CMD_FEATURES subcommand (02, 04 or 05) for mask */
size_t jc2_cmd_features(u8* out, size_t size, u8 sub, u8 mask);

/* a reply: the command and subcommand it answers, its status (1 for done), and its data */
typedef struct {
    u8 cmd, status, sub;
    const u8* data;
    size_t size;
} Jc2Reply;
int jc2_reply(const u8* p, size_t n, Jc2Reply* r); /* 0 when p is shorter than a header */
/* a flash read's reply: the bytes read at address, at least n of them; NULL when it isn't that read */
const u8* jc2_reply_flash(const Jc2Reply* r, u32 address, u8 n);

/* ---- the controller's flash */
#define JC2_FLASH_INFO       0x13000u /* 0x40: the serial, ids and colors */
#define JC2_FLASH_GYRO       0x13040u /* 0x10: the gyroscope's zero offsets */
#define JC2_FLASH_STICK      0x13080u /* 0x40: the stick's factory block, its calibration at +0x28 */
#define JC2_FLASH_STICK2     0x130c0u /* 0x40: the second stick's (a two-stick controller's right one), the same way */
#define JC2_FLASH_ACCEL      0x13100u /* 0x18: the accelerometer's */
#define JC2_FLASH_USER_STICK 0x1fc040u /* 0x40: the user's stick calibration, behind B2 A1, the second stick's at +0x20 */
typedef struct {
    char serial[17];
    u16 vendor, product;
    u8 body[3], buttons[3]; /* RGB */
} Jc2Info;
int jc2_info(const u8* p, size_t n, Jc2Info* info); /* 0 when n is short of 0x22 */
/* a stick's calibration, 12-bit: the centre, and the travel above and below it, X then Y */
typedef struct {
    u16 center[2], above[2], below[2];
} Jc2StickCal;
/* the 9 packed bytes; 0 when they are absent (all 0xFF) or give no travel */
int jc2_stick_cal(const u8 b[9], Jc2StickCal* c);
/* the user's copy at JC2_FLASH_USER_STICK (+0 the stick of a Joy-Con), valid behind its B2 A1 magic */
int jc2_user_stick_cal(const u8 b[11], Jc2StickCal* c);
/* a raw 12-bit axis as -32767..32767, by a calibration (axis 0 X, 1 Y) */
i32 jc2_stick_axis(u16 raw, const Jc2StickCal* c, int axis);
/* the gyroscope's zero offsets in rad/s, the 0x10 bytes at JC2_FLASH_GYRO; 0 when they are out of range */
int jc2_gyro_cal(const u8 p[16], float offset[3]);

/* ---- the common input report (JC2_H_COMMON), 63 bytes */
#define JC2_REPORT_SIZE 63u
typedef struct {
    u32 counter;
    u64 buttons; /* NpadButton bits: A 0 ... Down 15, SL/SR of the left 24/25, of the right 26/27 */
    u8 extra;    /* the buttons the Npad has no bit for: JC2_X_* */
    u16 stick[2][2];   /* raw 12-bit, the left stick's X and Y, then the right's */
    u16 mouse[2];      /* the optical sensor's running counters, X and Y */
    u8 mouse_surface;  /* 0x14: the sensor's distance to what it sees, 0xff for nothing (JC2_SURFACE_NEAR) */
    u16 battery_mv;
    i16 accel[3], gyro[3]; /* raw: 4096 per g, 16.4 per degree/s */
    u32 imu_us;            /* the IMU sample's time, microseconds */
} Jc2Report;
#define JC2_X_HOME    0x01u
#define JC2_X_CAPTURE 0x02u
#define JC2_X_C       0x04u
#define JC2_X_GL      0x08u
#define JC2_X_GR      0x10u
/* the report's fields of the features in mask (JC2_F_*); a feature that is off reads as zero. 0 when n is short */
int jc2_report(const u8* p, size_t n, u32 mask, Jc2Report* r);
/* a sensor counter's motion since prev, -32768..32767 */
i32 jc2_mouse_delta(u16 now, u16 prev);
/* the IMU sample on the console's six-axis axes, both hands: acceleration in g as the state has it (the IMU's
   reading negated: -1 on Z lying face up), angular velocity in rad/s (PadMotion's unit) less the zero offsets */
void jc2_motion(const Jc2Report* r, const float gyro_offset[3], float accel[3], float gyro[3]);

/* ---- the advert's manufacturer data, after the company id */
typedef struct {
    u16 product;
    u8 host[6];  /* the console it is paired with, byte-reversed; all zero while it waits to sync */
    int syncing; /* its host is all zero */
    int wake;
} Jc2Advert;
int jc2_advert(const u8* p, size_t n, Jc2Advert* a); /* 0 when it isn't a Joy-Con 2's or a Pro Controller 2's */

/* ---- HD rumble: one actuator's 16-byte block, a state byte then three 5-byte samples. A sample is the low band's
   frequency and amplitude, then the high band's, 10 bits each (little-endian) */
typedef struct {
    float amp_low, freq_low, amp_high, freq_high; /* as a VibrationValue: 0..1, Hz */
} Jc2Vibration;
/* the strongest band the host sends. The console's encoder scales the two bands to a sum of 0.98 and applies limits
   per device type that were not traced. 0.45, as cautious as the public tools are, clipped 326 of 906 writes in one
   game that asks the player to find where the vibration is strongest, so all of its strong field felt the
   same: the band keeps the sum's limit alone */
#define JC2_RUMBLE_CAP    0.98f
#define JC2_RUMBLE_PACKET 42u   /* a vibration-only write: 00, the block, zeros */
u16 jc2_rumble_freq(float hz); /* round(96 log2(hz) - 318.01), hz within 40..3500 */
u16 jc2_rumble_amp(float a);   /* round(a * 1023), a within 0..1 */
void jc2_rumble_pack(u8 out[5], u16 freq_low, u16 amp_low, u16 freq_high, u16 amp_high);
/* v as the controller plays it: the bands' sum kept to 0.98, each to JC2_RUMBLE_CAP */
void jc2_rumble_limit(Jc2Vibration* v);
/* the packet for handle JC2_H_VIBRATION: v (limited) in all three samples, counter's low 4 bits in the state byte */
void jc2_rumble_packet(u8 out[JC2_RUMBLE_PACKET], const Jc2Vibration* v, unsigned counter);
/* the same for a Pro Controller 2, whose two actuators each have a block: the left one's, then the right one's. Same
   handle (its characteristics share the Joy-Con 2's handle numbers), the rest of the 42 bytes zero */
void jc2_rumble_packet_pro(u8 out[JC2_RUMBLE_PACKET], const Jc2Vibration* left, const Jc2Vibration* right,
                           unsigned counter);
#endif
