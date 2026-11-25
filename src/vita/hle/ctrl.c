/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceCtrl and SceTouch: the buttons, the sticks and the touch panels, read from the player's input when asked */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "common/utilities/endian.h"
#include "vita/platform/input.h"
#include <string.h>

#define SCE_CTRL_ERROR_INVALID_ARG 0x80340001u
#define SCE_TOUCH_ERROR_INVALID_ARG 0x80350001u

/* SceCtrlData (32 bytes): the time in microseconds, the buttons, the sticks (lx, ly, rx, ry), then the pressure of
   each button (0 or 255 here) */
static void ctrl_data(VitaProcess* p, uint32_t at, const VitaInput* in) {
    unsigned char* b = vita_ptr(p, at);
    memset(b, 0, 32);
    vita_write64(p, at, (plat_ns() - p->start_ns) / 1000u);
    vita_write32(p, at + 8, in->buttons);
    b[12] = in->lx, b[13] = in->ly, b[14] = in->rx, b[15] = in->ry;
    static const uint32_t pressure[12] = {VITA_CTRL_UP,       VITA_CTRL_RIGHT,    VITA_CTRL_DOWN,     VITA_CTRL_LEFT,
                                          VITA_CTRL_LTRIGGER, VITA_CTRL_RTRIGGER, 0,                  0,
                                          VITA_CTRL_TRIANGLE, VITA_CTRL_CIRCLE,   VITA_CTRL_CROSS,    VITA_CTRL_SQUARE};
    for (unsigned i = 0; i < 12; ++i)
        if (pressure[i] && (in->buttons & pressure[i])) b[16 + i] = 255;
}
/* sceCtrlPeekBufferPositive(port, data, count): the state now, in each of count entries */
uint32_t vita_ctrl_peek_buffer_positive(VitaProcess* p, VitaThread* t, uint32_t port, uint32_t data, uint32_t count) {
    (void)t, (void)port;
    if (!data || count < 1 || count > 64 || !vita_mapped(p, data, count * 32)) return SCE_CTRL_ERROR_INVALID_ARG;
    VitaInput in;
    vita_input_read(&in);
    for (uint32_t i = 0; i < count; ++i) ctrl_data(p, data + i * 32, &in);
    return count;
}
/* the read variant waits for the next sample, which the console takes each vblank: the state now, after it */
uint32_t vita_ctrl_read_buffer_positive(VitaProcess* p, VitaThread* t, uint32_t port, uint32_t data, uint32_t count) {
    uint32_t r = vita_ctrl_peek_buffer_positive(p, t, port, data, count);
    if (r & 0x80000000u) return r;
    t->cpu.r[0] = r;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_VBLANK, .a = (uint32_t)p->display.vblanks, .b = 1});
    return r;
}
uint32_t vita_ctrl_set_sampling_mode(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1; /* the mode before: SCE_CTRL_MODE_ANALOG */
}
/* SceTouchData (144 bytes): the time, a status, the report count, then each report (16 bytes: id, force, x, y) */
uint32_t vita_ctrl_touch_peek(VitaProcess* p, VitaThread* t, uint32_t port, uint32_t data, uint32_t count) {
    (void)t;
    if (port > 1 || !data || count < 1 || count > 64 || !vita_mapped(p, data, count * 144))
        return SCE_TOUCH_ERROR_INVALID_ARG;
    VitaInput in;
    vita_input_read(&in);
    bool down = port ? in.back : in.front;
    uint16_t x = port ? in.back_x : in.front_x, y = port ? in.back_y : in.front_y;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = data + i * 144;
        unsigned char* b = vita_ptr(p, at);
        memset(b, 0, 144);
        vita_write64(p, at, (plat_ns() - p->start_ns) / 1000u);
        vita_write32(p, at + 12, down ? 1 : 0);
        if (down) {
            b[16] = 0, b[17] = 128; /* id, force */
            snx_st_le16(b + 18, (uint16_t)x), snx_st_le16(b + 20, (uint16_t)y);
        }
    }
    return count;
}
uint32_t vita_ctrl_touch_set_sampling_state(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* SceTouchPanelInfo: the panel's minimum and maximum active and physical coordinates, then the force range */
uint32_t vita_ctrl_touch_get_panel_info(VitaProcess* p, VitaThread* t, uint32_t port, uint32_t info) {
    (void)t;
    if (port > 1 || !info) return SCE_TOUCH_ERROR_INVALID_ARG;
    uint16_t height = port ? 890 : 1088;
    uint16_t values[10] = {0, 0, 1919, height - 1, 0, 0, 1919, height - 1, 1, 128};
    unsigned char* b = vita_ptr(p, info);
    memset(b, 0, 32);
    for (unsigned i = 0; i < 10; ++i) snx_st_le16(b + i * 2, values[i]);
    return 0;
}
