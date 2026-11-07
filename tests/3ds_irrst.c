/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ir:rst (services/irrst.c), the New 3DS's ZL, ZR and C-stick: its replies, as Azahar's ir_rst.cpp gives them, and the
   samples its shared memory gets from Initialize to Shutdown, with an SDL virtual gamepad's triggers and right stick
   as the player's input under SDL's dummy video driver */
#include "3ds/input/input.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/irrst.h"
#include "common/utilities/endian.h"
#include "support/3ds_guest.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "3ds_irrst.c:%d: check failed: %s\n", __LINE__, #e);                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the shared memory's index, its first entry, and the C-stick's direction bits in the state */
enum { INDEX = 0x10, ENTRIES = 0x18, ENTRY_SIZE = 0x10 };
#define C_RIGHT (1u << 24)
#define C_UP    (1u << 26)
#define PAD_IDS "0x045e/0x02ea" /* the virtual pad's vendor and product: a real pad plugged in stays out */

/* a field of 2 or 4 bytes of ir:rst's shared memory, little endian, as the game reads it */
static uint32_t shared(SnxCtrProcess* p, uint32_t offset, unsigned size) {
    unsigned char bytes[4] = {0};
    uint32_t pa = p->objects[p->irrst_objects[0]].data.shared.physical;
    CHECK(snx_ctr_phys_read(p->memory, pa + offset, bytes, size) == SNX_CTR_MEM_OK);
    return snx_ld_le32(bytes);
}
static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) snx_ctr_input_event(&e);
}
/* the triggers (0 released, 1 held) and the right stick (-1 to 1, y up) */
static void hold(SDL_Joystick* pad, int left, int right, float x, float y) {
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                               left ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                               right ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)(x * 32767));
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY, (Sint16)(-y * 32767));
    pump();
}
/* the next sample, at its deadline: the index it wrote */
static uint32_t sample(SnxCtrProcess* p) {
    p->objects[p->irrst_objects[1]].data.event.signaled = 0;
    snx_ctr_irrst_update(p, snx_ctr_irrst_deadline(p));
    CHECK(p->objects[p->irrst_objects[1]].data.event.signaled);
    return shared(p, INDEX, 4);
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, PAD_IDS);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    snx_ctr_input_init();
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = 0x045e;
    d.product_id = 0x02ea;
    SDL_Joystick* pad = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(pad != NULL);
    hold(pad, 0, 0, 0, 0);

    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_NEW);
    CHECK(p != NULL);
    /* srv: registered as the application, then GetServiceHandle */
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0};
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    CHECK(ctr_command_word(p, 0) == 0x00010040 && !ctr_command_word(p, 1));
    uint32_t rst = ctr_service_handle(p, srv, "ir:rst");
    CHECK(snx_ctr_irrst_deadline(p) == UINT64_MAX); /* nothing samples before Initialize */
    /* GetHandles: the shared memory and the event, moved out in one descriptor, as Azahar's PushMoveObjects; a second
       call hands out new handles to the same two */
    CHECK(ctr_send_header(p, rst, 0x00010000) == 0x00010043 && ctr_command_word(p, 2) == 0x04000010);
    uint32_t memory = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    uint32_t event = snx_ctr_handle_object(p, ctr_command_word(p, 4));
    CHECK(memory && memory == p->irrst_objects[0] && p->objects[memory].type == CTR_OBJ_SHARED);
    CHECK(event && event == p->irrst_objects[1] && p->objects[event].type == CTR_OBJ_EVENT);
    CHECK(p->objects[memory].data.shared.size == 4096 && p->objects[memory].data.shared.permissions == SNX_CTR_READ);
    CHECK(ctr_send_header(p, rst, 0x00010000) == 0x00010043);
    CHECK(snx_ctr_handle_object(p, ctr_command_word(p, 3)) == memory &&
          snx_ctr_handle_object(p, ctr_command_word(p, 4)) == event);
    CHECK(snx_ctr_irrst_deadline(p) == UINT64_MAX);

    /* Initialize: a sample every 10 ms, the C-stick with its direction bits */
    const uint32_t every_10ms[3] = {0x00020080, 10, 0};
    ctr_call(p, rst, every_10ms, 3, SNX_CTR_SVC_RETURN, 0);
    CHECK(ctr_command_word(p, 0) == 0x00020040 && !ctr_command_word(p, 1));
    uint64_t due = snx_ctr_irrst_deadline(p);
    CHECK(due != UINT64_MAX && p->irrst.period_ms == 10);
    /* ZL (the left trigger) and the C-stick right (the right stick): entry 0, all of it new, the position at the rim,
       the ticks of the index's wrap to 0, and the next sample 10 ms on */
    hold(pad, 1, 0, 1, 0);
    CHECK(sample(p) == 0);
    CHECK(shared(p, ENTRIES, 4) == (SNX_CTR_PAD_ZL | C_RIGHT) &&
          shared(p, ENTRIES + 4, 4) == (SNX_CTR_PAD_ZL | C_RIGHT) && !shared(p, ENTRIES + 8, 4));
    CHECK((int16_t)shared(p, ENTRIES + 12, 2) == 0x9c && !shared(p, ENTRIES + 14, 2));
    CHECK(shared(p, 0, 4) | shared(p, 4, 4));
    CHECK(snx_ctr_irrst_deadline(p) == due + 10000000u);
    /* ZR and the C-stick up: entry 1, ZL and right let go of */
    hold(pad, 0, 1, 0, 1);
    CHECK(sample(p) == 1);
    CHECK(shared(p, ENTRIES + ENTRY_SIZE, 4) == (SNX_CTR_PAD_ZR | C_UP) &&
          shared(p, ENTRIES + ENTRY_SIZE + 4, 4) == (SNX_CTR_PAD_ZR | C_UP) &&
          shared(p, ENTRIES + ENTRY_SIZE + 8, 4) == (SNX_CTR_PAD_ZL | C_RIGHT));
    CHECK(!shared(p, ENTRIES + ENTRY_SIZE + 12, 2) && (int16_t)shared(p, ENTRIES + ENTRY_SIZE + 14, 2) == 0x9c);
    /* the right stick within gamepad_deadzone's 20%: the C-stick at rest, no direction */
    hold(pad, 0, 1, 0.15f, 0);
    CHECK(sample(p) == 2 && shared(p, ENTRIES + 2 * ENTRY_SIZE, 4) == SNX_CTR_PAD_ZR &&
          shared(p, ENTRIES + 2 * ENTRY_SIZE + 8, 4) == C_UP && !shared(p, ENTRIES + 2 * ENTRY_SIZE + 12, 4));

    /* Initialize again asking for 0 ms and the raw C-stick, as Azahar does not model: a sample every millisecond from
       entry 0, the position without its direction bits, changes against the entry last written */
    hold(pad, 0, 1, 0, -1);
    const uint32_t raw_now[3] = {0x00020080, 0, 1};
    ctr_call(p, rst, raw_now, 3, SNX_CTR_SVC_RETURN, 0);
    CHECK(ctr_command_word(p, 0) == 0x00020040 && p->irrst.period_ms == 1 && p->irrst.raw_c_stick);
    CHECK(sample(p) == 0 && shared(p, ENTRIES, 4) == SNX_CTR_PAD_ZR && !shared(p, ENTRIES + 4, 4) &&
          !shared(p, ENTRIES + 8, 4) && (int16_t)shared(p, ENTRIES + 14, 2) == -0x9c);

    /* Shutdown stops the samples; WriteToTwoFields, which Azahar has no handler for, is answered by the stub */
    CHECK(ctr_send_header(p, rst, 0x00030000) == 0x00030040 && snx_ctr_irrst_deadline(p) == UINT64_MAX);
    CHECK(ctr_send_header(p, rst, 0x00090000) == 0x00090040);

    snx_ctr_process_free(p);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    pump();
    snx_ctr_input_free();
    SDL_Quit();
    printf("3ds ir:rst: %d checks\n", checks);
    return 0;
}
