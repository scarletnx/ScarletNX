/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
   hid:USER and hid:SPVR: the shared memory the game reads its input from and the events that say a sample is there,
   as a Switch game's hid writes its shared memory (nx/hle/services/hid.c). The samples, their layout and their pace
   are ported from Azahar's service/hid/hid.cpp and hid.h (GPL-2.0-or-later), checked against 3dbrew's HID Shared
   Memory. The input is the player's (input/input.c), the touch screen included. The sensors are at rest, the console
   lying flat. */
#include "3ds/services/hid.h"
#include "3ds/input/input.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "services/hid.swipc.inc"
#include <math.h>
#include <string.h>

/* Azahar's periods, at the ARM11's 268111856 Hz: the pad's 234 times a second, the accelerometer's 104 and the
   gyroscope's 101 */
#define HID_PAD_NS           UINT64_C(4273504)
#define HID_ACCELEROMETER_NS UINT64_C(9615382)
#define HID_GYROSCOPE_NS     UINT64_C(9900987)
#define HID_TICK_HZ          268111856.0
/* the shared memory's sections (3dbrew's HID Shared Memory): each starts with the tick of its last index wrap and
   the one before it, then the index last written */
#define HID_PAD           0x000u /* current_state 0x1c, entries[8] of 0x10 bytes from 0x28 */
#define HID_TOUCH         0x0a8u /* entries[8] of 8 bytes from 0x20 */
#define HID_ACCELEROMETER 0x108u /* raw entry 0x18, entries[8] of 6 bytes from 0x20 */
#define HID_GYROSCOPE     0x158u /* raw entry 0x18, entries[32] of 6 bytes from 0x20 */
#define HID_CIRCLE_MAX    0x9a   /* the circle pad's rim */
/* the circle pad's direction bits, past HID_CIRCLE_DEAD of its position */
#define HID_CIRCLE_RIGHT (1u << 28)
#define HID_CIRCLE_LEFT  (1u << 29)
#define HID_CIRCLE_UP    (1u << 30)
#define HID_CIRCLE_DOWN  (1u << 31)
#define HID_CIRCLE_DEAD  40

static uint32_t shared_physical(const SnxCtrProcess* p) {
    return p->hid_objects[0] ? p->objects[p->hid_objects[0]].data.shared.physical : 0;
}
static void put(SnxCtrProcess* p, uint32_t offset, const void* bytes, size_t size) {
    (void)snx_ctr_phys_write(p->memory, shared_physical(p) + offset, bytes, size);
}
static void put16(SnxCtrProcess* p, uint32_t offset, uint16_t v) {
    unsigned char b[2];
    snx_st_le16(b, v);
    put(p, offset, b, 2);
}
static void put32(SnxCtrProcess* p, uint32_t offset, uint32_t v) {
    unsigned char b[4];
    snx_st_le32(b, v);
    put(p, offset, b, 4);
}
static uint32_t get32(SnxCtrProcess* p, uint32_t offset) {
    unsigned char b[4] = {0};
    (void)snx_ctr_phys_read(p->memory, shared_physical(p) + offset, b, 4);
    return snx_ld_le32(b);
}
/* the system tick, as svcGetSystemTick counts it */
static uint64_t ticks(const SnxCtrProcess* p) {
    double elapsed = snx_seconds() - p->clock_origin;
    return elapsed > 0 ? (uint64_t)(elapsed * HID_TICK_HZ) : 0;
}
/* The section's next index, written once its entry is: at a wrap to 0 the ticks move on, the current one becoming the
   previous */
static void advance(SnxCtrProcess* p, uint32_t section, uint32_t index) {
    if (!index) {
        unsigned char current[8];
        (void)snx_ctr_phys_read(p->memory, shared_physical(p) + section, current, 8);
        put(p, section + 8, current, 8);
        uint64_t now = ticks(p);
        put32(p, section, (uint32_t)now);
        put32(p, section + 4, (uint32_t)(now >> 32));
    }
    put32(p, section + 0x10, index);
}
static void signal_event(SnxCtrProcess* p, uint32_t event) {
    uint32_t id = p->hid_objects[event];
    if (!id) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
/* The circle pad's direction bits, as Azahar's GetStickDirectionState gives them: none near the center, else right
   or left within 60 degrees of the horizontal and up or down within 60 of the vertical, two on a diagonal */
static uint32_t circle_directions(int32_t x, int32_t y) {
    if (x * x + y * y <= HID_CIRCLE_DEAD * HID_CIRCLE_DEAD) return 0;
    const float tan30 = 0.577350269f, tan60 = 1.732050808f;
    float t = x ? fabsf((float)y / (float)x) : 0;
    uint32_t bits = 0;
    if (x && t < tan60) bits |= x > 0 ? HID_CIRCLE_RIGHT : HID_CIRCLE_LEFT;
    if (!x || t > tan30) bits |= y > 0 ? HID_CIRCLE_UP : HID_CIRCLE_DOWN;
    return bits;
}
/* Azahar's UpdatePadCallback: the pad's entry, with what was pressed and released since the one before it, the touch
   screen's entry, then both pad events */
static void update_pad(SnxCtrProcess* p) {
    SnxCtrInput input;
    snx_ctr_input_read(&input);
    SnxCtrHid* h = &p->hid;
    int32_t x = (int32_t)roundf(input.circle[0] * HID_CIRCLE_MAX), y = (int32_t)roundf(input.circle[1] * HID_CIRCLE_MAX);
    /* the position is the mean of this one and the two before it, as Azahar's CIRCLE_PAD_AVERAGING */
    int32_t mean_x = (x + h->circle_history[0][0] + h->circle_history[1][0]) / 3;
    int32_t mean_y = (y + h->circle_history[0][1] + h->circle_history[1][1]) / 3;
    h->circle_history[1][0] = h->circle_history[0][0], h->circle_history[1][1] = h->circle_history[0][1];
    h->circle_history[0][0] = x, h->circle_history[0][1] = y;
    uint32_t state = (input.buttons & SNX_CTR_PAD_BUTTONS) | circle_directions(mean_x, mean_y);
    uint32_t index = h->pad_index, entry = HID_PAD + 0x28 + index * 0x10;
    uint32_t old = get32(p, HID_PAD + 0x28 + ((index + 7) & 7) * 0x10), changed = state ^ old;
    put32(p, entry, state);
    put32(p, entry + 4, changed & state);
    put32(p, entry + 8, changed & old);
    put16(p, entry + 12, (uint16_t)(int16_t)mean_x);
    put16(p, entry + 14, (uint16_t)(int16_t)mean_y);
    put32(p, HID_PAD + 0x1c, state);
    advance(p, HID_PAD, index);
    h->pad_index = (index + 1) & 7;
    index = h->touch_index;
    entry = HID_TOUCH + 0x20 + index * 8;
    /* valid: 1 while the screen is pressed, 0 after a release with the last position kept */
    put16(p, entry, (uint16_t)input.touch_x);
    put16(p, entry + 2, (uint16_t)input.touch_y);
    put32(p, entry + 4, input.touch);
    advance(p, HID_TOUCH, index);
    h->touch_index = (index + 1) & 7;
    signal_event(p, 1);
    signal_event(p, 2);
}
/* Azahar's UpdateAccelerometerCallback with the console at rest, gravity along -y: 512 a g, and the raw entry
   Azahar makes from it */
static void update_accelerometer(SnxCtrProcess* p) {
    static const int16_t at_rest[3] = {0, -512, 0};
    uint32_t index = p->hid.accelerometer_index;
    for (unsigned i = 0; i < 3; ++i) put16(p, HID_ACCELEROMETER + 0x20 + index * 6 + i * 2, (uint16_t)at_rest[i]);
    put16(p, HID_ACCELEROMETER + 0x18, (uint16_t)(int16_t)(-2 * at_rest[0]));
    put16(p, HID_ACCELEROMETER + 0x1a, (uint16_t)(int16_t)(-2 * at_rest[2]));
    put16(p, HID_ACCELEROMETER + 0x1c, (uint16_t)(int16_t)(2 * at_rest[1]));
    advance(p, HID_ACCELEROMETER, index);
    p->hid.accelerometer_index = (index + 1) & 7;
    signal_event(p, 3);
}
/* Azahar's UpdateGyroscopeCallback with the console still: every rate 0 */
static void update_gyroscope(SnxCtrProcess* p) {
    static const unsigned char still[6] = {0};
    uint32_t index = p->hid.gyroscope_index;
    put(p, HID_GYROSCOPE + 0x20 + index * 6, still, 6);
    put(p, HID_GYROSCOPE + 0x18, still, 6);
    advance(p, HID_GYROSCOPE, index);
    p->hid.gyroscope_index = (index + 1) & 31;
    signal_event(p, 4);
}
/* the deadline one period on; one more than a period behind the host (a slow slice) starts again from now instead of
   delivering the missed samples back to back, as the vblank does */
static void next(uint64_t* deadline, uint64_t period, uint64_t now) {
    *deadline += period;
    if (*deadline + period < now) *deadline = now + period;
}
void snx_ctr_hid_update(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrHid* h = &p->hid;
    if (h->next_pad_ns && now_ns >= h->next_pad_ns) {
        update_pad(p);
        next(&h->next_pad_ns, HID_PAD_NS, now_ns);
    }
    if (h->next_accelerometer_ns && now_ns >= h->next_accelerometer_ns) {
        update_accelerometer(p);
        next(&h->next_accelerometer_ns, HID_ACCELEROMETER_NS, now_ns);
    }
    if (h->next_gyroscope_ns && now_ns >= h->next_gyroscope_ns) {
        update_gyroscope(p);
        next(&h->next_gyroscope_ns, HID_GYROSCOPE_NS, now_ns);
    }
}
uint64_t snx_ctr_hid_deadline(const SnxCtrProcess* p) {
    uint64_t deadline = UINT64_MAX;
    const uint64_t each[] = {p->hid.next_pad_ns, p->hid.next_accelerometer_ns, p->hid.next_gyroscope_ns};
    for (unsigned i = 0; i < 3; ++i)
        if (each[i] && each[i] < deadline) deadline = each[i];
    return deadline;
}
static uint32_t snx_ctr_hid_GetIPCHandles(SnxCtrIpc* c, uint32_t* memory, uint32_t* pad1, uint32_t* pad2,
                                          uint32_t* accelerometer, uint32_t* gyroscope, uint32_t* debug_pad) {
    SnxCtrProcess* p = c->process;
    uint32_t* out[] = {memory, pad1, pad2, accelerometer, gyroscope, debug_pad};
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t result = p->hid_objects[0] ? snx_ctr_handle_add(p, p->hid_objects[i], out[i])
                                            : snx_ctr_object_create(p, i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED, out[i]);
        if (result) return result; /* generated output cleanup is transactional */
    }
    if (!p->hid_objects[0]) {
        uint32_t physical = snx_ctr_shared_pages(p, 4096);
        if (!physical) return 0xd86007f3u;
        for (unsigned i = 0; i < 6; ++i) {
            p->hid_objects[i] = snx_ctr_handle_object(p, *out[i]);
            ++p->objects[p->hid_objects[i]].refs;
        }
        /* One HLE system-pool page, outside the application pool/accounting, zeroed as board RAM is: the first pad
           sample is one period on, as the HID module's first is after it starts */
        CtrObject* shared = &p->objects[p->hid_objects[0]];
        shared->data.shared.physical = physical;
        shared->data.shared.size = 4096;
        shared->data.shared.permissions = SNX_CTR_READ;
        p->hid.next_pad_ns = snx_ctr_now_ns() + HID_PAD_NS;
    }
    return 0;
}
/* Each sensor samples from its first enable to its last disable (Azahar counts them) */
static uint32_t snx_ctr_hid_EnableAccelerometer(SnxCtrIpc* c) {
    SnxCtrHid* h = &c->process->hid;
    if (!h->accelerometer_users++) h->next_accelerometer_ns = snx_ctr_now_ns() + HID_ACCELEROMETER_NS;
    return 0;
}
static uint32_t snx_ctr_hid_DisableAccelerometer(SnxCtrIpc* c) {
    SnxCtrHid* h = &c->process->hid;
    if (h->accelerometer_users && !--h->accelerometer_users) h->next_accelerometer_ns = 0;
    return 0;
}
static uint32_t snx_ctr_hid_EnableGyroscopeLow(SnxCtrIpc* c) {
    SnxCtrHid* h = &c->process->hid;
    if (!h->gyroscope_users++) h->next_gyroscope_ns = snx_ctr_now_ns() + HID_GYROSCOPE_NS;
    return 0;
}
static uint32_t snx_ctr_hid_DisableGyroscopeLow(SnxCtrIpc* c) {
    SnxCtrHid* h = &c->process->hid;
    if (h->gyroscope_users && !--h->gyroscope_users) h->next_gyroscope_ns = 0;
    return 0;
}
/* 14.375 (0x41660000) raw units a degree a second, the gyroscope's (Azahar's gyroscope_coef) */
static uint32_t snx_ctr_hid_GetGyroscopeLowRawToDpsCoefficient(SnxCtrIpc* c, uint32_t* coefficient) {
    (void)c;
    *coefficient = 0x41660000u;
    return 0;
}
/* Each axis's {zero 0, positive 6700, negative -6700} as s16s, three of them in 18 bytes padded to 20, as Azahar
   answers */
static uint32_t snx_ctr_hid_GetGyroscopeLowCalibrateParam(SnxCtrIpc* c, uint32_t* param0, uint32_t* param1,
                                                          uint32_t* param2, uint32_t* param3, uint32_t* param4) {
    (void)c;
    const uint16_t axis[3] = {0, 6700, (uint16_t)-6700};
    uint16_t halves[10] = {0};
    for (unsigned i = 0; i < 9; ++i) halves[i] = axis[i % 3];
    uint32_t* words[] = {param0, param1, param2, param3, param4};
    for (unsigned i = 0; i < 5; ++i) *words[i] = (uint32_t)halves[2 * i] | (uint32_t)halves[2 * i + 1] << 16;
    return 0;
}
/* the volume slider's position, 0 to 0x3f: at the top */
static uint32_t snx_ctr_hid_GetSoundVolume(SnxCtrIpc* c, uint32_t* volume) {
    (void)c;
    *volume = 0x3f;
    return 0;
}
