/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceDisplay: the framebuffer the screen shows, taken at the next vblank, and the waits for vblanks */
#include "vita/hle/hle.h"
#include "vita/gpu/present.h"

#define SCE_DISPLAY_ERROR_INVALID_VALUE 0x80290001u
#define SCE_DISPLAY_ERROR_INVALID_ADDR  0x80290002u

/* sceDisplaySetFrameBuf(SceDisplayFrameBuf* param, sync): size, base, pitch (pixels), format, width, height. NULL
   blanks the screen */
static uint32_t set_frame_buf(VitaProcess* p, VitaThread* t) {
    uint32_t param = vita_arg(p, t, 0);
    if (!param) {
        p->display.base = 0;
        return 0;
    }
    uint32_t base = vita_read32(p, param + 4), pitch = vita_read32(p, param + 8);
    uint32_t format = vita_read32(p, param + 12), width = vita_read32(p, param + 16), height = vita_read32(p, param + 20);
    if (!width || !height || width > pitch || width > 960 || height > 544) return SCE_DISPLAY_ERROR_INVALID_VALUE;
    if (!vita_mapped(p, base, pitch * height * 4)) return SCE_DISPLAY_ERROR_INVALID_ADDR;
    p->display.base = base, p->display.pitch = pitch, p->display.format = format;
    p->display.width = width, p->display.height = height;
    return 0;
}
static uint32_t get_frame_buf(VitaProcess* p, VitaThread* t) {
    uint32_t param = vita_arg(p, t, 0);
    if (!param) return SCE_DISPLAY_ERROR_INVALID_VALUE;
    vita_write32(p, param + 4, p->display.base);
    vita_write32(p, param + 8, p->display.pitch);
    vita_write32(p, param + 12, p->display.format);
    vita_write32(p, param + 16, p->display.width);
    vita_write32(p, param + 20, p->display.height);
    return 0;
}
static uint32_t wait_vblank(VitaProcess* p, VitaThread* t, uint32_t count) {
    p->pacer = t;
    p->display.due_count = count ? count : 1;
    p->display.due = p->display.vblanks + p->display.due_count;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_VBLANK, .a = (uint32_t)p->display.vblanks + (count ? count - 1 : 0)});
    return 0;
}
/* A game paces itself by waiting count vblanks once a frame. The console waits from the call, so a frame whose work runs
   past a vblank waits one more and a game that fits 30 frames a second there drops to 20. A wait here counts from the
   vblank the thread's last wait ended at. A frame that ran late then waits only for the vblank it was due at, or not at
   all when that has passed, and the next wait counts from where this one ended. A thread that has not waited for more
   than twice count vblanks waits from the call as before */
static uint32_t pace_vblank(VitaProcess* p, VitaThread* t, uint32_t count) {
    uint64_t now = p->display.vblanks, n = count ? count : 1, target = now + n;
    if (t->vblank_wake && now <= t->vblank_wake + 2 * n && t->vblank_wake + n < target) target = t->vblank_wake + n;
    p->pacer = t;
    p->display.due_count = (uint32_t)n;
    p->display.due = target > now ? target : now;
    if (target <= now) {
        t->vblank_wake = now;
        return 0;
    }
    t->vblank_wake = target;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_VBLANK, .a = (uint32_t)(target - 1)});
    return 0;
}
static uint32_t wait_vblank_start(VitaProcess* p, VitaThread* t) { return pace_vblank(p, t, 1); }
static uint32_t wait_vblank_start_multi(VitaProcess* p, VitaThread* t) { return pace_vblank(p, t, vita_arg(p, t, 0)); }
static uint32_t wait_set_frame_buf(VitaProcess* p, VitaThread* t) { return wait_vblank(p, t, 1); }
static uint32_t wait_set_frame_buf_multi(VitaProcess* p, VitaThread* t) { return wait_vblank(p, t, vita_arg(p, t, 0)); }

bool vita_display_vblank(VitaProcess* p) {
    vita_gxm_poll(p);
    return vita_present_vblank(p);
}

/* sceDisplayGetRefreshRate(float* rate): the screen's 59.94005 Hz */
static uint32_t get_refresh_rate(VitaProcess* p, VitaThread* t) {
    uint32_t at = vita_arg(p, t, 0);
    if (at && vita_mapped(p, at, 4)) vita_write32(p, at, 0x426fc29cu);
    return 0;
}
const VitaHle vita_hle_display[] = {
    {0x7a410b64u, "sceDisplaySetFrameBuf", set_frame_buf},
    {0xa08ca60du, "sceDisplayGetRefreshRate", get_refresh_rate},
    {0x42ae6bbcu, "sceDisplayGetFrameBuf", get_frame_buf},
    {0x5795e898u, "sceDisplayWaitVblankStart", wait_vblank_start},
    {0x78b41b92u, "sceDisplayWaitVblankStartCB", wait_vblank_start},
    {0xdd0a13b8u, "sceDisplayWaitVblankStartMulti", wait_vblank_start_multi},
    {0x9423560cu, "sceDisplayWaitSetFrameBuf", wait_set_frame_buf},
    {0x7d9864a8u, "sceDisplayWaitSetFrameBufMulti", wait_set_frame_buf_multi},
    {0, NULL, NULL},
};
