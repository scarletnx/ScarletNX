/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One Joy-Con 2 on a connected, unpaired link: the commands that set it up, one at a time (a reply, or 10 s and up to
   5 sends), then its input reports as a pad's state, and the game's vibration out. No OS: the link is the
   transport's two writes, which the caller passes in, and the notifications it feeds back (joycon2_proto.h) */
#ifndef SNX_JOYCON2_SESSION_H
#define SNX_JOYCON2_SESSION_H
#include "nx/runtime/platform/joycon2_proto.h"

/* the link: a write without response to a handle, and a CCCD write (with response) turning its notifications on or
   off. Each returns 0 when the bytes went */
typedef struct {
    void* link;
    int (*write)(void* link, u16 handle, const u8* data, size_t n);
    int (*notify)(void* link, u16 cccd, int on);
} Jc2Transport;

#define JC2_REPLY_TIMEOUT_NS 10000000000ull /* the console's */
#define JC2_SENDS            5
#define JC2_GUARD_NS         10000000ull /* a pause after each reply before the next command */
#define JC2_RUMBLE_GAP_NS    15000000ull /* a vibration write at most this often (three samples in it) */
/* the report's surface byte (0x14) is a distance, not a flag. Measured on one right Joy-Con 2, 2026-11-15: lying on a
   table as a mouse about 10 (below 0x30 throughout), a palm over the sensor about 115, held in the hand about 200,
   lifted 0xff (no reading). The left one lying flat, its sensor to the side, about 0x35. Below this it is on a
   surface the game may take it as a mouse on */
#define JC2_SURFACE_NEAR     0x30
enum { JC2_SIDE_LEFT, JC2_SIDE_RIGHT, JC2_SIDE_PRO }; /* a left or right Joy-Con 2, or a Pro Controller 2 */
enum { JC2_FAILED = -1, JC2_SETUP = 0, JC2_READY = 1 };

/* the pad as the report last gave it, in the Switch's terms */
typedef struct {
    u64 buttons;      /* NpadButton bits, upright */
    u8 extra;         /* JC2_X_* */
    i32 stick[2];     /* its one stick (a Pro Controller 2's left one), X and Y up, -32767..32767 */
    i32 rstick[2];    /* a Pro Controller 2's right stick, the same way */
    float accel[3];   /* g */
    float gyro[3];    /* rad/s */
    u16 battery_mv;
    u8 on_surface;    /* the mouse sensor sees a surface: the Joy-Con lies on it as a mouse */
    u32 reports;      /* the reports taken */
} Jc2Pad;

typedef struct {
    int state;           /* JC2_FAILED, JC2_SETUP, JC2_READY */
    int side;            /* JC2_SIDE_*, from the advert's product id until the version reply says */
    int step;            /* the setup's step (0 first) */
    int sends;           /* of the step's command */
    u64 sent_ns;         /* when it went, 0 while none waits for its reply */
    u64 reply_ns;        /* when the last reply came */
    u8 version[3];
    Jc2Info info;
    Jc2StickCal stick;   /* the user's, else the factory's */
    int has_stick;
    Jc2StickCal stick2;  /* a Pro Controller 2's right stick's */
    float gyro_offset[3];
    Jc2Pad pad;
    u16 mouse_last[2];
    int mouse_seen;      /* a report gave the counters: the next one's motion counts */
    i32 mouse[2];        /* the sensor's motion not yet taken */
    u8 led, led_sent;    /* the player lights wanted, and sent (0 none yet) */
    Jc2Vibration rumble; /* the vibration wanted (a Pro Controller 2's left actuator) */
    Jc2Vibration rumble2; /* a Pro Controller 2's right actuator's */
    int rumble_on;       /* the last write played one: a stop is due when it ends */
    u64 rumble_ns;
    u32 rumble_reports; /* pad.reports at the last vibration write: one write a report, so none queue on the link */
    unsigned rumble_counter;
} Jc2Session;

/* a new controller's session: side from its advert's product id (JC2_SIDE_PRO: a Pro Controller 2, set up with its
   own features, its second stick's calibration, no mouse sensor and no Joy-Con actuator parameters) */
void jc2_session_start(Jc2Session* s, int side);
/* time passes: the next command when its turn comes, a send again after the timeout, the failure after the last;
   and the vibration due. Returns s->state. The times compare by sum, not difference: a notification stamped a little
   after now_ns (another clock read) makes no wait end early */
int jc2_session_tick(Jc2Session* s, const Jc2Transport* t, u64 now_ns);
/* a notification of handle: a reply that ends the step it answers, or an input report */
void jc2_session_notify(Jc2Session* s, const Jc2Transport* t, u16 handle, const u8* p, size_t n, u64 now_ns);
/* the player lights' pattern (bits 0-3), sent once the setup is done */
void jc2_session_led(Jc2Session* s, u8 pattern);
/* the vibration the game wants now, which the ticks write. A Pro Controller 2 has a second actuator, the right one */
void jc2_session_rumble(Jc2Session* s, const Jc2Vibration* v);
void jc2_session_rumble2(Jc2Session* s, const Jc2Vibration* right);
/* the sensor's motion since the last take, X then Y, in its counts */
void jc2_session_mouse(Jc2Session* s, i32 out[2]);
#endif
