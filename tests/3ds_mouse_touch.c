/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presenter's window: a pointer press on the bottom screen's blit rectangle becomes a hid:USER touch sample, a
   drag moves it and keeps to the screen's edge when it leaves it, and a release or a focus loss ends it, while another
   window's events do nothing. The samples are read from a process's shared memory as the game reads them. Without a
   Vulkan device and a window there is nothing to point at, and a window that is not the presenter's 600x720 leaves the
   fixture's window fractions meaningless: these skip with 77. */
#include "3ds/gpu/present.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/input/input.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/hid.h"
#include "common/utilities/endian.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>

#define FRAMEBUFFER       0x18000000u
#define FRAMEBUFFER_PITCH 960u /* 240 pixels of 4 bytes */
#define WINDOW_W          600
#define WINDOW_H          720
#define SMALL_W           400
#define SMALL_H           480
#define COMMAND           (SNX_CTR_INITIAL_TLS + 0x80)
/* the touch section of hid:USER's shared memory: its entry index, and the entries of 8 bytes (x, y, valid) */
enum {
    TOUCH_SECTION = 0xa8,
    TOUCH_INDEX = TOUCH_SECTION + 0x10,
    TOUCH_ENTRIES = TOUCH_SECTION + 0x20,
    TOUCH_ENTRY_SIZE = 8,
    TOUCH_VALID = 4
};

static uint32_t command_word(SnxCtrProcess* p, uint32_t index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return snx_ld_le32(b);
}
static uint32_t command_send(SnxCtrProcess* p, uint32_t session, uint32_t header) {
    unsigned char bytes[4];
    snx_st_le32(bytes, header);
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, 4) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!command_word(p, 1));
    return command_word(p, 0);
}
/* a 2-byte or 4-byte field of hid:USER's shared memory at offset, little endian, as the game reads it */
static uint32_t hid_shared(SnxCtrProcess* p, uint32_t offset, uint32_t size) {
    unsigned char b[4] = {0};
    uint32_t pa = p->objects[p->hid_objects[0]].data.shared.physical;
    assert(snx_ctr_phys_read(p->memory, pa + offset, b, size) == SNX_CTR_MEM_OK);
    return size == 2 ? snx_ld_le16(b) : snx_ld_le32(b);
}
/* a mouse event of a window, pushed and pumped as the presenter's loop pumps them */
static void event_mouse(Uint32 type, SDL_WindowID id, float x, float y) {
    SDL_Event e = {0};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.windowID = id;
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.windowID = id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    }
    assert(SDL_PushEvent(&e));
    assert(!snx_ctr_present_closed()); /* pumps the queue */
}
/* runs the HID clock to the next pad sample, then reads that sample's touch entry: x, y, and whether it is valid */
static bool next_touch_sample(SnxCtrProcess* p, uint16_t* x, uint16_t* y) {
    snx_ctr_hid_update(p, p->hid.next_pad_ns);
    uint32_t at = TOUCH_ENTRIES + hid_shared(p, TOUCH_INDEX, 4) * TOUCH_ENTRY_SIZE;
    *x = hid_shared(p, at, 2);
    *y = hid_shared(p, at + 2, 2);
    return hid_shared(p, at + TOUCH_VALID, 4) == 1;
}
/* the next sample is a press at x and y; a mismatch prints the sample before the assert fails */
static void touching(SnxCtrProcess* p, uint16_t x, uint16_t y) {
    uint16_t at_x = 0, at_y = 0;
    bool valid = next_touch_sample(p, &at_x, &at_y);
    if (!valid || at_x != x || at_y != y)
        fprintf(stderr, "touch sample (%u, %u), valid %d: expected (%u, %u), valid 1\n", (unsigned)at_x, (unsigned)at_y,
                valid, (unsigned)x, (unsigned)y);
    assert(valid && at_x == x && at_y == y);
}
/* the next sample is not a press */
static void no_touch(SnxCtrProcess* p) {
    uint16_t x, y;
    assert(!next_touch_sample(p, &x, &y));
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    snx_ctr_input_init();
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t session = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &session));
    assert(command_send(p, session, 0x000a0000) == 0x000a0047); /* the shared memory and the five events */
    int status = 77;
    char why[256];
    if (!snx_ctr_vk_init(p->memory, why, sizeof why)) {
        fprintf(stderr, "no Vulkan device (%s): skipped\n", why);
        goto done;
    }
    if (!snx_ctr_vk()->host.window) {
        fprintf(stderr, "no window to point at: skipped\n");
        goto done;
    }
    assert(snx_ctr_present_init(p->memory, NULL) && snx_ctr_present_windowed());
    SDL_Window* window = snx_ctr_vk()->host.window;
    SDL_WindowID id = SDL_GetWindowID(window);
    int w = 0, h = 0;
    assert(SDL_GetWindowSize(window, &w, &h));
    if (w != WINDOW_W || h != WINDOW_H) {
        fprintf(stderr, "the fixture needs the presenter's %dx%d window, not %dx%d: skipped\n", WINDOW_W, WINDOW_H, w,
                h);
        goto done;
    }
    /* a press before the first frame: no bottom screen is drawn yet, so there is no touch */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w / 2.0f, h * 0.75f);
    /* the screens as a game draws them: 400x240 above 320x240, the pair 600x720 at this window's size */
    assert(snx_ctr_present_frame(1, FRAMEBUFFER, 240, 320, 0, FRAMEBUFFER_PITCH, NULL));
    assert(snx_ctr_present_frame(0, FRAMEBUFFER, 240, 400, 0, FRAMEBUFFER_PITCH, NULL));
    assert(SDL_GetWindowSize(window, &w, &h) && w == WINDOW_W && h == WINDOW_H);
    /* at an unscaled 600x720 window the bottom screen is 480x360 at (60, 360), its centre (300, 540): 1.5x */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.3f, h * 0.625f);
    touching(p, 80, 60);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.101f, h * 0.501f); /* the top left corner */
    touching(p, 0, 0);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.899f, h * 0.999f); /* the bottom right corner */
    touching(p, 319, 239);
    /* a drag that leaves the bottom screen keeps to its edge, and follows the pointer again when it comes back */
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.05f, h * 0.75f); /* left of it: the left edge */
    touching(p, 0, 120);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.75f, h * 0.25f); /* above it, on the top screen: the top edge */
    touching(p, 260, 0);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w * 0.75f, h * 1.01f); /* below it: the bottom edge */
    touching(p, 260, 239);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    /* a press on the top screen is not a touch, and dragging it down onto the bottom one does not start one */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.25f);
    no_touch(p);
    event_mouse(SDL_EVENT_MOUSE_MOTION, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    /* a release outside the bottom screen is still the end of the press */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w * 0.05f, h * 0.75f);
    no_touch(p);
    /* another window's press, drag and release change nothing */
    SDL_Window* other = SDL_CreateWindow("other", 200, 200, SDL_WINDOW_HIDDEN);
    assert(other);
    SDL_WindowID other_id = SDL_GetWindowID(other);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, other_id, w / 2.0f, h * 0.75f);
    no_touch(p);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_MOTION, other_id, w * 0.3f, h * 0.625f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, other_id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    /* losing the focus ends the press the window can no longer follow */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    SDL_Event focus = {0};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.window.windowID = id;
    assert(SDL_PushEvent(&focus));
    assert(!snx_ctr_present_closed());
    no_touch(p);
    SDL_DestroyWindow(other);
    /* a window of another size with the same aspect ratio: the frame at the new size recreates the swapchain once the
       driver reports it out of date (present.c), and the window fractions still map onto the bottom screen's centre */
    assert(SDL_SetWindowSize(window, SMALL_W, SMALL_H));
    assert(SDL_SyncWindow(window));
    assert(snx_ctr_present_frame(0, FRAMEBUFFER, 240, 400, 0, FRAMEBUFFER_PITCH, NULL));
    /* the first present after a resize only rebuilds an out-of-date swapchain, so the second one shows the frame */
    assert(snx_ctr_present_frame(0, FRAMEBUFFER, 240, 400, 0, FRAMEBUFFER_PITCH, NULL));
    assert(SDL_GetWindowSize(window, &w, &h) && w == SMALL_W && h == SMALL_H);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, w / 2.0f, h * 0.75f);
    touching(p, 160, 120);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, w / 2.0f, h * 0.75f);
    no_touch(p);
    puts("the pointer reached the touch screen's HID samples");
    status = 0;
done:
    snx_ctr_present_free();
    snx_ctr_process_free(p);
    snx_ctr_input_free();
    return status;
}
