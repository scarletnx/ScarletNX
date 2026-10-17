/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/hid_core/resources/npad/npad.cpp, npad_types.h, src/hid_core/resources/shared_memory_format.h,
   ring_lifo.h, src/hid_core/hid_types.h, src/hid_core/hid_core.cpp, src/hid_core/hid_result.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project (GPL-2.0-or-later)
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* hid and hid:sys: the controllers as the game reads them from the shared memory (hid_layout.h). Each of the
   frontend's eight players (g_pad, g_pad_extra) is the Npad with its number, with exactly one style, the one its
   controller is that the game supports; the handheld Npad mirrors player 1 while undocked. A host's combined Joy-Cons
   can be split into two Npads, and two single Joy-Cons merged into one, as the game asks */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/hid_layout.h"
#include "nx/hle/services/hid_motion.h"

#include "hle/services/hid.swipc.inc"

PadState g_pad;
PadState g_pad_extra[7];
TouchState g_touch;
PMutex g_pad_input_lock = PMUTEX_INIT;

#define HID_INVALID_NPAD_ID RC(202, 709)

#define HID_PADS    8  /* the frontend's players */
#define HID_ENTRIES 10 /* No1 to No8, Handheld, Other: the shared memory's Npads */
#define HID_LIFO_N  17 /* each LIFO's entries */

/* nn::hid::NpadStyleTag */
#define STYLE_FULL_KEY  (1u << 0)
#define STYLE_HANDHELD  (1u << 1)
#define STYLE_JOY_DUAL  (1u << 2)
#define STYLE_JOY_LEFT  (1u << 3)
#define STYLE_JOY_RIGHT (1u << 4)
#define STYLE_ALL       0x7fu

/* the buttons of each half of a pair of Joy-Cons (nn::hid::NpadButton) */
#define BUTTONS_LEFT  0x30ff950ull /* StickL, L, ZL, Minus, the d-pad, the left stick's directions, Left SL/SR */
#define BUTTONS_RIGHT 0xcf006afull /* A, B, X, Y, StickR, R, ZR, Plus, the right stick's directions, Right SL/SR */
#define STICK_PRESS   0x4000       /* a stick past half its travel presses its direction */

/* where an Npad's input comes from */
enum { SRC_NONE, SRC_PAD, SRC_LEFT_HALF, SRC_RIGHT_HALF, SRC_MERGED, SRC_HANDHELD };
typedef struct {
    int kind;
    int p, q; /* the player (the left Joy-Con when merged), and the right Joy-Con's when merged */
} HidSource;

static struct {
    PMutex lock;
    KShmem *shm, *xpad_shm, *six_shm, *joysix_shm;
    KEvent* style_ev[HID_ENTRIES];
    u64 samp;    /* the sampling number of the latest states */
    u64 last_ns; /* when they were taken */
    u32 supported_styles, supported_ids; /* the game's: styles, and Npads by entry index */
    i64 hold, handheld_mode, comm_mode;
    PadState pad[HID_PADS]; /* the frontend's players, as the last update saw them */
    int split[HID_PADS];    /* a combined pair split: 1 + the Npad its other half is, 0 when it isn't */
    int keep_right[HID_PADS];
    int merged[HID_PADS]; /* a single Joy-Con merged with another: 1 + the other's player, on the one shown */
    u32 style[HID_ENTRIES];
    HidMotionState motion[HID_PADS][2];
    HidFingers fingers;
    nn_hid_VibrationValue vibration[HID_PADS][2];
} hid;

static int hid_entry(u32 npad_id) {
    if (npad_id < 8) return (int)npad_id;
    if (npad_id == 0x20) return 8;
    if (npad_id == 0x10) return 9;
    return -1;
}
static u32 hid_npad_id(int e) { return e < 8 ? (u32)e : e == 8 ? 0x20u : 0x10u; }
static u8* hid_npad(int e) { return kshmem_ptr(hid.shm) + HO_HidSharedMemory_npad + (u64)e * HS_HidNpadSharedMemoryEntry; }
static int hid_connected(int p) { return p == 0 || hid.pad[p].connected; } /* player 1 is the keyboard's too */
static int hid_single(int p) { return hid.pad[p].kind == SNX_PAD_JOY_LEFT || hid.pad[p].kind == SNX_PAD_JOY_RIGHT; }
/* the player merged into p's Npad, -1 for none; and the one whose Npad p's Joy-Con is merged into */
static int hid_partner(int p) { return hid.merged[p] ? hid.merged[p] - 1 : -1; }
static int hid_merged_into(int p) {
    int q;
    for (q = 0; q < HID_PADS; q++)
        if (hid.merged[q] == p + 1) return q;
    return -1;
}

/* ============================================================ the Npads' layout */
static void hid_sources(HidSource src[HID_ENTRIES]) {
    int p, q;
    memset(src, 0, sizeof(HidSource) * HID_ENTRIES);
    for (p = 0; p < HID_PADS; p++) {
        if (!hid_connected(p) || hid_merged_into(p) >= 0) continue;
        if ((q = hid_partner(p)) >= 0) {
            src[p].kind = SRC_MERGED;
            src[p].p = hid.pad[p].kind == SNX_PAD_JOY_LEFT ? p : q;
            src[p].q = hid.pad[p].kind == SNX_PAD_JOY_LEFT ? q : p;
        } else if (hid.split[p]) {
            src[p].kind = hid.keep_right[p] ? SRC_RIGHT_HALF : SRC_LEFT_HALF;
            src[p].p = p;
            src[hid.split[p] - 1].kind = hid.keep_right[p] ? SRC_LEFT_HALF : SRC_RIGHT_HALF;
            src[hid.split[p] - 1].p = p;
        } else {
            src[p].kind = SRC_PAD;
            src[p].p = p;
        }
    }
    if (!g_opt_docked) src[8].kind = SRC_HANDHELD;
}
/* the one style an Npad has: its controller's when the game supports it, else the nearest the game does */
static u32 hid_style_of(int e, const HidSource* s) {
    static const u32 prefer[4][3] = {{STYLE_FULL_KEY, STYLE_JOY_DUAL, 0},
                                     {STYLE_JOY_DUAL, STYLE_FULL_KEY, 0},
                                     {STYLE_JOY_LEFT, STYLE_FULL_KEY, STYLE_JOY_DUAL},
                                     {STYLE_JOY_RIGHT, STYLE_FULL_KEY, STYLE_JOY_DUAL}};
    u32 ok = hid.supported_styles;
    int i;
    if (!((hid.supported_ids >> e) & 1)) return 0;
    switch (s->kind) {
    case SRC_PAD:
        for (i = 0; i < 3; i++)
            if (prefer[hid.pad[s->p].kind][i] & ok) return prefer[hid.pad[s->p].kind][i];
        return 0;
    case SRC_LEFT_HALF: return ok & STYLE_JOY_LEFT;
    case SRC_RIGHT_HALF: return ok & STYLE_JOY_RIGHT;
    case SRC_MERGED: return ok & STYLE_JOY_DUAL;
    case SRC_HANDHELD: return ok & STYLE_HANDHELD;
    default: return 0;
    }
}

static void w32(u8* p, u32 v) { memcpy(p, &v, 4); }
/* an Npad's header for its new style: colors, assignment, device type, properties, as the console sets them */
static void hid_write_header(int e, u32 style) {
    u8* n = hid_npad(e);
    u64 props = 0;
    u32 device = 0, footer = 0, i;
    static const u32 body = 0xff323232u, buttons = 0xffe6e6e6u;
    w32(n + HO_HidNpadInternalState_style_set, style);
    w32(n + HO_HidNpadInternalState_joy_assignment_mode, (style & (STYLE_JOY_LEFT | STYLE_JOY_RIGHT)) ? 1 : 0);
    memset(n + HO_HidNpadInternalState_full_key_color, 0, HS_HidNpadFullKeyColorState + HS_HidNpadJoyColorState);
    w32(n + HO_HidNpadInternalState_full_key_color, style ? 0 : 2); /* ColorAttribute: Ok, NoController */
    w32(n + HO_HidNpadInternalState_joy_color, (style & ~STYLE_FULL_KEY) ? 0 : 2);
    if (style) {
        w32(n + HO_HidNpadInternalState_full_key_color + 4, body);
        w32(n + HO_HidNpadInternalState_full_key_color + 8, buttons);
    }
    if (style & ~STYLE_FULL_KEY)
        for (i = 0; i < 2; i++) {
            w32(n + HO_HidNpadInternalState_joy_color + 4 + 8 * i, body);
            w32(n + HO_HidNpadInternalState_joy_color + 8 + 8 * i, buttons);
        }
    /* DeviceType bits, NpadSystemProperties (vertical 11, horizontal 12, plus 13, minus 14, d-pad 15), the footer */
    switch (style) {
    case STYLE_FULL_KEY:
        device = 1u << 0;
        props = 1u << 11 | 1u << 13 | 1u << 14;
        footer = 12; /* SwitchProController */
        break;
    case STYLE_HANDHELD:
        device = 1u << 2 | 1u << 3;
        props = 1u << 11 | 1u << 13 | 1u << 14 | 1u << 15;
        footer = 4; /* HandheldJoyConLeftJoyConRight */
        break;
    case STYLE_JOY_DUAL:
        device = 1u << 4 | 1u << 5;
        props = 1u << 11 | 1u << 13 | 1u << 14 | 1u << 15;
        footer = 5;
        break;
    case STYLE_JOY_LEFT:
        device = 1u << 4;
        props = 1u << 12 | 1u << 14;
        footer = 8; /* JoyLeftHorizontal */
        break;
    case STYLE_JOY_RIGHT:
        device = 1u << 5;
        props = 1u << 12 | 1u << 13;
        footer = 10;
        break;
    default: break;
    }
    w32(n + HO_HidNpadInternalState_device_type, device);
    memcpy(n + HO_HidNpadInternalState_system_properties, &props, 8);
    for (i = 0; i < 3; i++) w32(n + HO_HidNpadInternalState_battery_level + 4 * i, style ? 4 : 0); /* Full */
    n[HO_HidNpadInternalState_applet_footer_ui_type] = (u8)footer;
    /* each sensor of the new style is newly assigned (SixAxisSensorProperties, after the lark and lucia types) */
    memset(n + HO_HidNpadInternalState_lark_type_l_and_main + 0x10, 0, 6);
    for (i = 0; i < 5; i++)
        if (style & (1u << i)) n[HO_HidNpadInternalState_lark_type_l_and_main + 0x10 + i + (i > 2)] = 1;
    if (style & STYLE_JOY_DUAL) n[HO_HidNpadInternalState_lark_type_l_and_main + 0x13] = 1;
}

/* splits and merges that no longer hold: a half's Npad the game doesn't take or a player took, a pair whose
   Joy-Cons aren't a left and a right any more. Under the lock */
static void hid_validate(void) {
    int p, d, q;
    for (p = 0; p < HID_PADS; p++) {
        if (hid.split[p]) {
            d = hid.split[p] - 1;
            if (!hid_connected(p) || hid.pad[p].kind != SNX_PAD_JOY_DUAL || hid_connected(d) ||
                !((hid.supported_ids >> d) & 1) || !((hid.supported_ids >> p) & 1))
                hid.split[p] = 0;
        }
        if ((q = hid_partner(p)) >= 0 &&
            (!hid_connected(p) || !hid_connected(q) || !hid_single(p) || !hid_single(q) ||
             hid.pad[p].kind == hid.pad[q].kind))
            hid.merged[p] = 0;
    }
}
/* the styles in the shared memory, and the game told of each change. Under the lock */
static void hid_layout(void) {
    HidSource src[HID_ENTRIES];
    int e;
    u32 style;
    hid_validate();
    hid_sources(src);
    for (e = 0; e < HID_ENTRIES; e++) {
        style = hid_style_of(e, &src[e]);
        if (style == hid.style[e]) continue;
        hid.style[e] = style;
        hid_write_header(e, style);
        kevent_signal(hid.style_ev[e]);
    }
}

/* ============================================================ LIFOs */
/* the next state of a LIFO of entries of size n (the sampling stamp, then the state): the stamp is odd while the
   entry is written and twice the sampling number once it is whole, so a reader can tell a torn copy */
static void hid_lifo_push(u8* lifo, u32 n, const void* state, u32 state_size) {
    u64 tail, count, stamp;
    u8* entry;
    memcpy(&tail, lifo + HO_HidCommonLifoHeader_tail, 8);
    memcpy(&count, lifo + HO_HidCommonLifoHeader_count, 8);
    tail = (tail + 1) % HID_LIFO_N;
    entry = lifo + 0x20 + tail * n;
    stamp = hid.samp << 1 | 1;
    __atomic_store_n((u64*)entry, stamp, __ATOMIC_RELEASE);
    memcpy(entry + 8, state, state_size);
    __atomic_store_n((u64*)entry, hid.samp << 1, __ATOMIC_RELEASE);
    stamp = HID_LIFO_N;
    memcpy(lifo + HO_HidCommonLifoHeader_buffer_count, &stamp, 8);
    if (count < HID_LIFO_N - 1) count++;
    memcpy(lifo + HO_HidCommonLifoHeader_count, &count, 8);
    __atomic_store_n((u64*)(lifo + HO_HidCommonLifoHeader_tail), tail, __ATOMIC_RELEASE);
}

/* a sideways single Joy-Con's stick (as the host reports it) held upright, and back */
static void stick_upright(i32* x, i32* y, int left) {
    i32 ox = *x, oy = *y;
    *x = left ? oy : -oy;
    *y = left ? -ox : ox;
}
static void stick_sideways(i32* x, i32* y, int left) {
    i32 ox = *x, oy = *y;
    *x = left ? -oy : oy;
    *y = left ? ox : -ox;
}
static u64 stick_buttons(i32 x, i32 y, int shift) {
    u64 b = 0;
    if (x < -STICK_PRESS) b |= 1ull << shift;
    if (y > STICK_PRESS) b |= 2ull << shift;
    if (x > STICK_PRESS) b |= 4ull << shift;
    if (y < -STICK_PRESS) b |= 8ull << shift;
    return b;
}

/* an Npad's NpadCommonState (HS_HidNpadCommonState bytes) from its source, in its style */
static void hid_npad_state(const HidSource* s, u32 style, u8* out) {
    const PadState* a = &hid.pad[s->p];
    i32 st[4] = {0, 0, 0, 0};
    u64 buttons = 0;
    u32 attr = 0;
    int horizontal = hid.hold == 1;
    memset(out, 0, HS_HidNpadCommonState);
    if (!style) return;
    switch (s->kind) {
    case SRC_PAD:
    case SRC_HANDHELD:
        buttons = a->buttons;
        st[0] = a->lx, st[1] = a->ly, st[2] = a->rx, st[3] = a->ry;
        if (style == STYLE_JOY_LEFT) {
            st[2] = st[3] = 0;
            if (!horizontal) stick_upright(&st[0], &st[1], 1);
        } else if (style == STYLE_JOY_RIGHT) {
            st[0] = st[1] = 0;
            if (!horizontal) stick_upright(&st[2], &st[3], 0);
        } else if (style == STYLE_JOY_DUAL && a->kind == SNX_PAD_JOY_LEFT)
            stick_upright(&st[0], &st[1], 1);
        else if (style == STYLE_JOY_DUAL && a->kind == SNX_PAD_JOY_RIGHT)
            stick_upright(&st[2], &st[3], 0);
        break;
    case SRC_LEFT_HALF:
        buttons = a->buttons & BUTTONS_LEFT;
        st[0] = a->lx, st[1] = a->ly;
        if (horizontal) stick_sideways(&st[0], &st[1], 1);
        break;
    case SRC_RIGHT_HALF:
        buttons = a->buttons & BUTTONS_RIGHT;
        st[2] = a->rx, st[3] = a->ry;
        if (horizontal) stick_sideways(&st[2], &st[3], 0);
        break;
    case SRC_MERGED:
        buttons = hid.pad[s->p].buttons | hid.pad[s->q].buttons;
        st[0] = hid.pad[s->p].lx, st[1] = hid.pad[s->p].ly;
        st[2] = hid.pad[s->q].rx, st[3] = hid.pad[s->q].ry;
        stick_upright(&st[0], &st[1], 1);
        stick_upright(&st[2], &st[3], 0);
        break;
    default: return;
    }
    buttons |= stick_buttons(st[0], st[1], 16) | stick_buttons(st[2], st[3], 20);
    /* NpadAttribute: connected, wired, and each Joy-Con's */
    switch (style) {
    case STYLE_HANDHELD: attr = 0x3f; break;
    case STYLE_JOY_DUAL: attr = 1 | 4 | 16; break;
    case STYLE_JOY_LEFT: attr = 1 | 4; break;
    case STYLE_JOY_RIGHT: attr = 1 | 16; break;
    default: attr = 1; break;
    }
    memcpy(out + HO_HidNpadCommonState_sampling_number, &hid.samp, 8);
    memcpy(out + HO_HidNpadCommonState_buttons, &buttons, 8);
    memcpy(out + HO_HidNpadCommonState_analog_stick_l, st, 16);
    memcpy(out + HO_HidNpadCommonState_attributes, &attr, 4);
}

/* the player and the sensor or motor behind an Npad's left (0) or right (1) device; 0 when there is none */
static int hid_device(const HidSource* s, int dev, int* p, int* sensor) {
    switch (s->kind) {
    case SRC_PAD:
        *p = s->p;
        *sensor = hid_single(s->p) ? 0 : dev;
        return 1;
    case SRC_HANDHELD:
        *p = 0;
        *sensor = dev;
        return 1;
    case SRC_LEFT_HALF:
    case SRC_RIGHT_HALF:
        *p = s->p;
        *sensor = s->kind == SRC_RIGHT_HALF;
        return 1;
    case SRC_MERGED:
        *p = dev ? s->q : s->p;
        *sensor = 0;
        return 1;
    default: return 0;
    }
}
static void hid_six_axis(int e, u32 lifo, const HidSource* s, int dev, u64 dt) {
    u8 state[HS_HidSixAxisSensorState];
    int p, sensor;
    if (!hid_device(s, dev, &p, &sensor)) return;
    hid_motion_sample(&hid.motion[p][sensor], &hid.pad[p].motion[sensor], dt, hid.samp, state);
    hid_lifo_push(hid_npad(e) + lifo, HS_HidSixAxisSensorStateAtomicStorage, state, sizeof state);
}

/* ============================================================ the update */
/* the frontend's players, taken at 60 Hz: a replaced controller ends the splits and merges it was in, and starts
   its sensors over */
static void hid_take_input(TouchState* touch) {
    PadState now[HID_PADS];
    int p, q;
    pm_lock(&g_pad_input_lock);
    now[0] = g_pad;
    memcpy(now + 1, g_pad_extra, sizeof g_pad_extra);
    *touch = g_touch;
    pm_unlock(&g_pad_input_lock);
    for (p = 0; p < HID_PADS; p++) {
        if (now[p].source_id != hid.pad[p].source_id || now[p].kind != hid.pad[p].kind ||
            now[p].connected != hid.pad[p].connected) {
            hid.split[p] = 0;
            hid.merged[p] = 0;
            if ((q = hid_merged_into(p)) >= 0) hid.merged[q] = 0;
            memset(hid.motion[p], 0, sizeof hid.motion[p]);
        }
        hid.pad[p] = now[p];
    }
}

void hid_update(void) {
    static const u32 lifos[5] = {HO_HidNpadInternalState_full_key_lifo, HO_HidNpadInternalState_handheld_lifo,
                                 HO_HidNpadInternalState_joy_dual_lifo, HO_HidNpadInternalState_joy_left_lifo,
                                 HO_HidNpadInternalState_joy_right_lifo};
    HidSource src[HID_ENTRIES];
    TouchState touch;
    u8 state[HS_HidNpadCommonState], none[HS_HidNpadCommonState], other[HS_HidTouchScreenState];
    u8* shm;
    u64 now = plat_ns(), dt;
    int e, i;
    pm_lock(&hid.lock);
    hid_take_input(&touch);
    hid_layout();
    hid_sources(src);
    hid.samp++;
    dt = hid.last_ns ? now - hid.last_ns : 0;
    hid.last_ns = now;
    shm = kshmem_ptr(hid.shm);
    for (e = 0; e < HID_ENTRIES; e++) {
        u32 style = hid.style[e];
        u8* n = hid_npad(e);
        hid_npad_state(&src[e], style, state);
        memset(none, 0, sizeof none);
        memcpy(none, &hid.samp, 8);
        for (i = 0; i < 5; i++)
            hid_lifo_push(n + lifos[i], HS_HidNpadCommonStateAtomicStorage, style == 1u << i ? state : none,
                          HS_HidNpadCommonState);
        hid_lifo_push(n + HO_HidNpadInternalState_palma_lifo, HS_HidNpadCommonStateAtomicStorage, none,
                      HS_HidNpadCommonState);
        hid_lifo_push(n + HO_HidNpadInternalState_system_ext_lifo, HS_HidNpadCommonStateAtomicStorage,
                      style ? state : none, HS_HidNpadCommonState);
        switch (style) {
        case STYLE_FULL_KEY:
            hid_six_axis(e, HO_HidNpadInternalState_full_key_six_axis_sensor_lifo, &src[e], 0, dt);
            break;
        case STYLE_HANDHELD:
            hid_six_axis(e, HO_HidNpadInternalState_handheld_six_axis_sensor_lifo, &src[e], 0, dt);
            break;
        case STYLE_JOY_DUAL:
            hid_six_axis(e, HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo, &src[e], 0, dt);
            hid_six_axis(e, HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo, &src[e], 1, dt);
            break;
        case STYLE_JOY_LEFT:
            hid_six_axis(e, HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo, &src[e], 0, dt);
            break;
        case STYLE_JOY_RIGHT:
            hid_six_axis(e, HO_HidNpadInternalState_joy_right_six_axis_sensor_lifo, &src[e], 1, dt);
            break;
        default: break;
        }
    }
    /* the touch screen, and the devices nothing drives here, which only count their samples */
    hid_touch_sample(&hid.fingers, &touch, hid.samp, other);
    hid_lifo_push(shm + HO_HidSharedMemory_touchscreen + HO_HidTouchScreenSharedMemoryFormat_lifo,
                  HS_HidTouchScreenStateAtomicStorage, other, HS_HidTouchScreenState);
    memset(other, 0, sizeof other);
    memcpy(other, &hid.samp, 8);
    hid_lifo_push(shm + HO_HidSharedMemory_debug_pad, 8 + HS_HidDebugPadState, other, HS_HidDebugPadState);
    hid_lifo_push(shm + HO_HidSharedMemory_mouse + HO_HidMouseSharedMemoryFormat_lifo, 8 + HS_HidMouseState, other,
                  HS_HidMouseState);
    hid_lifo_push(shm + HO_HidSharedMemory_keyboard + HO_HidKeyboardSharedMemoryFormat_lifo, 8 + HS_HidKeyboardState,
                  other, HS_HidKeyboardState);
    pm_unlock(&hid.lock);
}

/* the Npads the game sees among No1 to No8 (the controller applet's player count), and the first it sees of all */
int hid_connected_players(u32* first_id) {
    int e, n = 0, first = -1;
    pm_lock(&hid.lock);
    for (e = 0; e < HID_ENTRIES; e++) {
        if (!hid.style[e]) continue;
        if (e < 8) n++;
        if (first < 0) first = e;
    }
    pm_unlock(&hid.lock);
    *first_id = first < 0 ? 0 : hid_npad_id(first);
    return n;
}

/* ============================================================ IHidServer */
static int nn_hid_IHidServer_CreateAppletResource(SvcObj* self, u64 aruid, SvcObj** resource) {
    (void)self, (void)aruid;
    *resource = nn_hid_IAppletResource_new(0);
    return 0;
}
static int nn_hid_IAppletResource_GetSharedMemoryHandle(SvcObj* self, u32* shared_memory) {
    (void)self;
    *shared_memory = h_new(&hid.shm->o);
    return 0;
}
static int nn_hid_IHidServer_AcquireXpadIdEventHandle(SvcObj* self, u64 unknown, u32* event) {
    (void)self, (void)unknown;
    return new_event(event, "hid:xpad id", 0);
}
static int nn_hid_IHidServer_GetJoyXpadLifoHandle(SvcObj* self, nn_hid_JoyXpadId id, u32* lifo) {
    (void)self, (void)id;
    *lifo = h_new(&hid.xpad_shm->o);
    return 0;
}
static int nn_hid_IHidServer_GetSixAxisSensorLifoHandle(SvcObj* self, nn_hid_BasicXpadId id, u32* lifo) {
    (void)self, (void)id;
    *lifo = h_new(&hid.six_shm->o);
    return 0;
}
static int nn_hid_IHidServer_GetJoySixAxisSensorLifoHandle(SvcObj* self, nn_hid_JoyXpadId id, u32* lifo) {
    (void)self, (void)id;
    *lifo = h_new(&hid.joysix_shm->o);
    return 0;
}
static int nn_hid_IHidServer_IsSixAxisSensorFusionEnabled(SvcObj* self, nn_hid_SixAxisSensorHandle handle, u64 aruid,
                                                          u8* enabled) {
    (void)self, (void)handle, (void)aruid;
    *enabled = 1;
    return 0;
}
/* a device handle (SixAxisSensorHandle, VibrationDeviceHandle): the style index, the Npad id, the device */
static int hid_handle_device(u32 handle, int* p, int* sensor) {
    HidSource src[HID_ENTRIES];
    int e = hid_entry(handle >> 8 & 0xff);
    if (e < 0) return 0;
    hid_sources(src);
    return hid_device(&src[e], (int)(handle >> 16 & 1), p, sensor);
}
static int nn_hid_IHidServer_IsSixAxisSensorAtRest(SvcObj* self, nn_hid_SixAxisSensorHandle handle, u64 aruid,
                                                   u8* at_rest) {
    PadMotion m;
    int p, sensor, found;
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    found = hid_handle_device(handle, &p, &sensor);
    pm_unlock(&hid.lock);
    if (!found) {
        *at_rest = 1;
        return 0;
    }
    pm_lock(&g_pad_input_lock);
    m = (p ? g_pad_extra[p - 1] : g_pad).motion[sensor];
    pm_unlock(&g_pad_input_lock);
    *at_rest = (u8)hid_motion_at_rest(&m);
    return 0;
}
static int nn_hid_IHidServer_SetSupportedNpadStyleSet(SvcObj* self, nn_hid_NpadStyleSet style_set, u64 aruid) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid.supported_styles = style_set & STYLE_ALL;
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_GetSupportedNpadStyleSet(SvcObj* self, u64 aruid, nn_hid_NpadStyleSet* style_set) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    *style_set = hid.supported_styles;
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_SetSupportedNpadIdType(SvcObj* self, u64 aruid, IpcBuf ids) {
    u32 id, mask = 0;
    u64 i;
    int e;
    (void)self, (void)aruid;
    for (i = 0; i + 4 <= ids.size; i += 4) {
        memcpy(&id, GP(ids.addr + i), 4);
        if ((e = hid_entry(id)) >= 0) mask |= 1u << e;
    }
    pm_lock(&hid.lock);
    hid.supported_ids = mask;
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_AcquireNpadStyleSetUpdateEventHandle(SvcObj* self, nn_hid_NpadIdType id, u64 aruid,
                                                                  u64 event_ptr, u32* event) {
    int e = hid_entry(id);
    (void)self, (void)aruid, (void)event_ptr;
    if (e < 0) return HID_INVALID_NPAD_ID;
    *event = ev_handle(hid.style_ev[e]);
    return 0;
}
/* the player lights: one more from the left for No1 to No4, then the console's patterns for No5 to No8 */
static int nn_hid_IHidServer_GetPlayerLedPattern(SvcObj* self, nn_hid_NpadIdType id, u64* pattern) {
    static const u8 leds[8] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6};
    (void)self;
    *pattern = id < 8 ? leds[id] : 0;
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyHoldType(SvcObj* self, u64 aruid, nn_hid_NpadJoyHoldType hold_type) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid.hold = hold_type;
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_GetNpadJoyHoldType(SvcObj* self, u64 aruid, nn_hid_NpadJoyHoldType* hold_type) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    *hold_type = hid.hold;
    pm_unlock(&hid.lock);
    return 0;
}

/* an Npad on its own: a merged pair goes back to its two players, a combined pair splits onto the first Npad the
   game takes that no one has (and stays whole if there is none). Returns the Npad the other half went to, -1 */
static int hid_assign_single(int e, int keep_right) {
    HidSource src[HID_ENTRIES];
    int p, d, q;
    if (e < 0 || e >= HID_PADS) return -1;
    if (hid.merged[e]) {
        hid.merged[e] = 0;
        return -1;
    }
    if ((q = hid_merged_into(e)) >= 0) {
        hid.merged[q] = 0;
        return -1;
    }
    hid_sources(src);
    if (src[e].kind != SRC_PAD || hid.pad[e].kind != SNX_PAD_JOY_DUAL) return -1;
    p = src[e].p;
    for (d = 0; d < HID_PADS; d++) {
        if (d == e || hid_connected(d) || !((hid.supported_ids >> d) & 1)) continue;
        for (q = 0; q < HID_PADS; q++)
            if (hid.split[q] == d + 1) break;
        if (q < HID_PADS) continue;
        hid.split[p] = d + 1;
        hid.keep_right[p] = keep_right;
        return d;
    }
    return -1;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingleByDefault(SvcObj* self, nn_hid_NpadIdType id, u64 aruid) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid_assign_single(hid_entry(id), 0);
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingle(SvcObj* self, nn_hid_NpadIdType id, u64 aruid,
                                                            nn_hid_NpadJoyDeviceType type) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid_assign_single(hid_entry(id), type == 1);
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingleWithDestination(SvcObj* self, nn_hid_NpadIdType id,
                                                                           u64 aruid, nn_hid_NpadJoyDeviceType type,
                                                                           u8* is_reassigned,
                                                                           nn_hid_NpadIdType* new_id) {
    int d;
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    d = hid_assign_single(hid_entry(id), type == 1);
    hid_layout();
    pm_unlock(&hid.lock);
    *is_reassigned = d >= 0;
    *new_id = d >= 0 ? (u32)d : id;
    return 0;
}
/* a split pair whole again, on the Npad its controller is */
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeDual(SvcObj* self, nn_hid_NpadIdType id, u64 aruid) {
    HidSource src[HID_ENTRIES];
    int e = hid_entry(id);
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid_sources(src);
    if (e >= 0 && (src[e].kind == SRC_LEFT_HALF || src[e].kind == SRC_RIGHT_HALF)) hid.split[src[e].p] = 0;
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
/* two single Joy-Cons, a left and a right, as one dual Npad on the first one's; any pair either was in ends */
static int nn_hid_IHidServer_MergeSingleJoyAsDualJoy(SvcObj* self, nn_hid_NpadIdType id1, nn_hid_NpadIdType id2,
                                                     u64 aruid) {
    int a = hid_entry(id1), b = hid_entry(id2), i, q;
    (void)self, (void)aruid;
    if (a < 0 || b < 0) return HID_INVALID_NPAD_ID;
    pm_lock(&hid.lock);
    if (a < HID_PADS && b < HID_PADS && a != b) {
        for (i = 0; i < 2; i++) {
            int p = i ? b : a;
            hid.merged[p] = 0;
            if ((q = hid_merged_into(p)) >= 0) hid.merged[q] = 0;
        }
        if (hid_connected(a) && hid_connected(b) && hid_single(a) && hid_single(b) && !hid.split[a] && !hid.split[b] &&
            hid.pad[a].kind != hid.pad[b].kind)
            hid.merged[a] = b + 1;
    }
    hid_layout();
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_SetNpadHandheldActivationMode(SvcObj* self, u64 aruid,
                                                           nn_hid_NpadHandheldActivationMode mode) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid.handheld_mode = mode;
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_GetNpadHandheldActivationMode(SvcObj* self, u64 aruid,
                                                           nn_hid_NpadHandheldActivationMode* mode) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    *mode = hid.handheld_mode;
    pm_unlock(&hid.lock);
    return 0;
}

/* ============================================================ vibration */
/* a linear resonant actuator on each side */
static int nn_hid_IHidServer_GetVibrationDeviceInfo(SvcObj* self, nn_hid_VibrationDeviceHandle handle,
                                                    nn_hid_VibrationDeviceInfo* info) {
    (void)self;
    info->type = 1;
    info->position = 1 + (handle >> 16 & 1);
    return 0;
}
static u32 hid_amplitude(float a) {
    if (!(a > 0.0f)) return 0; /* a NaN too */
    if (a >= 1.0f) return 65535;
    return (u32)(a * 65535.0f);
}
/* what the frontend plays for the motor behind the handle (hid.h's PadState.rumble). Under the lock */
static void hid_vibrate(u32 handle, const nn_hid_VibrationValue* v) {
    int p, motor;
    if (!hid_handle_device(handle, &p, &motor)) return;
    motor = (int)(handle >> 16 & 1);
    hid.vibration[p][motor] = *v;
    __atomic_store_n(&(p ? &g_pad_extra[p - 1] : &g_pad)->rumble[motor],
                     hid_amplitude(v->amp_low) | hid_amplitude(v->amp_high) << 16, __ATOMIC_RELAXED);
}
static int nn_hid_IHidServer_SendVibrationValue(SvcObj* self, nn_hid_VibrationDeviceHandle handle,
                                                const nn_hid_VibrationValue* value, u64 aruid) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid_vibrate(handle, value);
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_GetActualVibrationValue(SvcObj* self, nn_hid_VibrationDeviceHandle handle, u64 aruid,
                                                     nn_hid_VibrationValue* value) {
    int p, motor;
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    if (hid_handle_device(handle, &p, &motor))
        *value = hid.vibration[p][handle >> 16 & 1];
    else
        value->freq_low = 160.0f, value->freq_high = 320.0f;
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_CreateActiveVibrationDeviceList(SvcObj* self, SvcObj** list) {
    (void)self;
    *list = nn_hid_IActiveVibrationDeviceList_new(0);
    return 0;
}
static int nn_hid_IHidServer_IsVibrationPermitted(SvcObj* self, u8* permitted) {
    (void)self;
    *permitted = 1;
    return 0;
}
static int nn_hid_IHidServer_SendVibrationValues(SvcObj* self, nn_applet_AppletResourceUserId aruid, IpcBuf handles,
                                                 IpcBuf values) {
    nn_hid_VibrationValue v;
    u32 h;
    u64 i, n = handles.size / 4 < values.size / sizeof v ? handles.size / 4 : values.size / sizeof v;
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    for (i = 0; i < n; i++) {
        memcpy(&h, GP(handles.addr + 4 * i), 4);
        memcpy(&v, GP(values.addr + sizeof v * i), sizeof v);
        hid_vibrate(h, &v);
    }
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_IsVibrationDeviceMounted(SvcObj* self, nn_hid_VibrationDeviceHandle handle, u64 aruid,
                                                      u8* mounted) {
    int p, motor;
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    *mounted = (u8)hid_handle_device(handle, &p, &motor);
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_SetNpadCommunicationMode(SvcObj* self, u64 aruid, nn_hid_NpadCommunicationMode mode) {
    (void)self, (void)aruid;
    pm_lock(&hid.lock);
    hid.comm_mode = mode;
    pm_unlock(&hid.lock);
    return 0;
}
static int nn_hid_IHidServer_GetNpadCommunicationMode(SvcObj* self, nn_hid_NpadCommunicationMode* mode) {
    (void)self;
    pm_lock(&hid.lock);
    *mode = hid.comm_mode;
    pm_unlock(&hid.lock);
    return 0;
}

/* ============================================================ init */
static SvcObj* new_hid(void) { return nn_hid_IHidServer_new(0); }
static SvcObj* new_hid_sys(void) { return nn_hid_IHidSystemServer_new(0); }
static KShmem* hid_shm(u64 size, const char* name) {
    KShmem* s = kshmem_new(size, name);
    kobj_ref(&s->o); /* the service's, for as long as it runs */
    return s;
}
void svc_hid_init(void) {
    int e, p;
    pm_init(&hid.lock);
    hid.shm = hid_shm(HS_HidSharedMemory, "hid");
    hid.xpad_shm = hid_shm(0x1000, "hid:xpad lifo");
    hid.six_shm = hid_shm(0x1000, "hid:six-axis lifo");
    hid.joysix_shm = hid_shm(0x1000, "hid:joy six-axis lifo");
    /* signaled at first, so the game reads the styles it finds */
    for (e = 0; e < HID_ENTRIES; e++) {
        hid.style_ev[e] = owned_event("hid:npad style set update");
        kevent_signal(hid.style_ev[e]);
    }
    for (p = 0; p < HID_PADS; p++)
        for (e = 0; e < 2; e++) {
            hid.vibration[p][e].freq_low = 160.0f;
            hid.vibration[p][e].freq_high = 320.0f;
        }
    /* every style and every Npad until the game says which it takes; Joy-Cons held upright */
    hid.supported_styles = STYLE_ALL;
    hid.supported_ids = (1u << HID_ENTRIES) - 1;
    pm_lock(&hid.lock);
    hid_layout();
    pm_unlock(&hid.lock);
    nn_hid_IHidServer_register(new_hid);
    nn_hid_IHidSystemServer_register(new_hid_sys);
}
