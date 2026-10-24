/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The controllers as the frontend sees them, which hid turns into the samples in its shared memory */
#ifndef SNX_HID_H
#define SNX_HID_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"      /* KShmem, KEvent */
#include "nx/hle/services/hid_touch.h" /* TouchState, HidFingers */
#include "nx/runtime/platform/plat.h"  /* PMutex */

/* One motion sensor, in the Switch controller's frame: acceleration in g, angular velocity in rad/s */
typedef struct {
    float acceleration[3];
    float angular_velocity[3];
    int valid;
} PadMotion;

/* the physical controller behind a player */
typedef enum { SNX_PAD_FULL_KEY = 0, SNX_PAD_JOY_DUAL, SNX_PAD_JOY_LEFT, SNX_PAD_JOY_RIGHT } PadKind;

typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
    int connected; /* player 1 stays connected, for the keyboard and scripted input */
    PadKind kind;
    u32 source_id;       /* the SDL instance id, which changes when the controller is replaced */
    PadMotion motion[2]; /* the left (or only) sensor, and the right one when there is one */
    /* vibration the game asked for, left and right: amp_low in bits 0-15, amp_high in bits 16-31. Game threads write
       it and the frontend plays it */
    u32 rumble[2];
} PadState;

void hid_update(void); /* the next samples in shared memory, touch screen included */
/* the number of connected players, and the Npad id of the first in *first_id */
int hid_connected_players(u32* first_id);
/* ---- HLE state in g_hle (hle.h) */
#define HID_PADS    8  /* the frontend's players */
#define HID_ENTRIES 10 /* No1 to No8, Handheld, Other: the shared memory's Npads */

/* a motion sensor's orientation, which hid_motion_sample integrates from one sample to the next */
typedef struct {
    float quaternion[4]; /* scalar, x, y, z; zero state means identity */
    float angle[3];
    int was_valid;
} HidMotionState;
/* a vibration value as the game sent it (nn::hid::VibrationValue, laid out as hid.swipc's) */
typedef struct {
    float amp_low, freq_low, amp_high, freq_high;
} HidVibration;
typedef struct {
    /* The frontend publishes these and hid samples them, both under input_lock */
    PadState pad1;          /* player 1 */
    PadState pad_extra[7];  /* players 2 to 8, in Npad id order */
    TouchState touch;
    u32 shm, xpad_shm, six_shm, joysix_shm; /* kernel object ids */
    u32 style_ev[HID_ENTRIES];
    u64 samp;    /* the sampling number of the latest states */
    u64 last_ns; /* when they were taken */
    u32 supported_styles, supported_ids; /* the game's: styles, and Npads by entry index */
    i64 hold, handheld_mode, comm_mode;
    i64 header_hold; /* the hold type the single Joy-Cons' headers were written for */
    PadState pad[HID_PADS]; /* the frontend's players, as the last update saw them */
    int split[HID_PADS];    /* a combined pair split: 1 + the Npad its other half is, 0 when it isn't */
    int keep_right[HID_PADS];
    int merged[HID_PADS]; /* a single Joy-Con merged with another: 1 + the other's player, on the one shown */
    u32 style[HID_ENTRIES];
    HidMotionState motion[HID_PADS][2];
    HidFingers fingers;
    HidVibration vibration[HID_PADS][2];
} HleHid;
/* hid's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex input_lock;
    /* hid's own, under lock */
    PMutex lock;
} HostHid;
#endif
