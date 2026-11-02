/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's pads (gui/pad_menu.h): the button labelled A or Cross selects whichever face it is on, B or Circle goes
   back, and a pad's presses arrive as the keys they are, held arrows again after GUI_PAD_DELAY_MS, on SDL's virtual
   pads: an Xbox-like one, and a Switch Pro Controller's, whose A is east. */
#include "gui/pad_menu.h"
#include "common/utilities/pad_sdl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const int XBOX[4] = {SNX_PAD_LABEL_A, SNX_PAD_LABEL_B, SNX_PAD_LABEL_X, SNX_PAD_LABEL_Y};
static const int SWITCH[4] = {SNX_PAD_LABEL_B, SNX_PAD_LABEL_A, SNX_PAD_LABEL_Y, SNX_PAD_LABEL_X};
static const int PS[4] = {SNX_PAD_LABEL_CROSS, SNX_PAD_LABEL_CIRCLE, SNX_PAD_LABEL_SQUARE, SNX_PAD_LABEL_TRIANGLE};
static const int NONE[4] = {SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE};

static SDL_Keycode key_of(int control, const int labels[4]) {
    SDL_Keycode key;
    SDL_Keymod mod;
    bool repeats;
    return gui_pad_key(control, labels, &key, &mod, &repeats) ? key : SDLK_UNKNOWN;
}
/* a virtual pad of vendor and product plugged in, its id */
static SDL_JoystickID plug(Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = vendor;
    d.product_id = product;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "virtual pad";
    return SDL_AttachVirtualJoystick(&d);
}
/* the events SDL has for p, the key down of the last of them in *key: how many keys */
static int pump(GuiPads* p, Uint64 now, SDL_Event* key) {
    SDL_Event e;
    int keys = 0;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) keys += gui_pads_event(p, &e, now, key);
    return keys;
}
/* button of joystick j pressed, then let go: the key it pressed */
static SDL_Keycode tap(GuiPads* p, SDL_Joystick* j, int button) {
    SDL_Event key;
    SDL_zero(key);
    SDL_SetJoystickVirtualButton(j, button, true);
    assert(pump(p, 0, &key) == 1 && key.type == SDL_EVENT_KEY_DOWN && key.key.which == GUI_PAD_KEYBOARD);
    SDL_SetJoystickVirtualButton(j, button, false);
    assert(pump(p, 0, &key) == 0);
    return key.key.key;
}

int main(void) {
    GuiPads p;
    SDL_Event key;
    SDL_JoystickID xbox, pro;
    SDL_Joystick *jx, *jp;
    SDL_Keycode code;
    SDL_Keymod mod;
    bool repeats;
    int machine; /* the pads of the machine open */

    /* A or Cross selects, B or Circle goes back, wherever they are; a face with no labels selects with south */
    assert(key_of(SNX_PAD_SOUTH, XBOX) == SDLK_RETURN && key_of(SNX_PAD_EAST, XBOX) == SDLK_ESCAPE);
    assert(key_of(SNX_PAD_SOUTH, SWITCH) == SDLK_ESCAPE && key_of(SNX_PAD_EAST, SWITCH) == SDLK_RETURN);
    assert(key_of(SNX_PAD_SOUTH, PS) == SDLK_RETURN && key_of(SNX_PAD_EAST, PS) == SDLK_ESCAPE);
    assert(key_of(SNX_PAD_SOUTH, NONE) == SDLK_RETURN && key_of(SNX_PAD_EAST, NONE) == SDLK_ESCAPE);
    /* X or Square is Del and Y or Triangle R, wherever they are: a Switch pad has X north */
    assert(key_of(SNX_PAD_WEST, XBOX) == SDLK_DELETE && key_of(SNX_PAD_NORTH, XBOX) == SDLK_R);
    assert(key_of(SNX_PAD_WEST, SWITCH) == SDLK_R && key_of(SNX_PAD_NORTH, SWITCH) == SDLK_DELETE);
    assert(key_of(SNX_PAD_WEST, PS) == SDLK_DELETE && key_of(SNX_PAD_NORTH, PS) == SDLK_R);
    assert(key_of(SNX_PAD_WEST, NONE) == SDLK_DELETE && key_of(SNX_PAD_NORTH, NONE) == SDLK_R);
#ifdef SDL_PLATFORM_ANDROID
    assert(key_of(SNX_PAD_START, XBOX) == SDLK_F5 && key_of(SNX_PAD_BACK, XBOX) == SDLK_ESCAPE);
#else
    assert(key_of(SNX_PAD_START, XBOX) == SDLK_F5 && key_of(SNX_PAD_BACK, XBOX) == SDLK_F1);
#endif
    /* the foot's keys as the pad names them: the letters of a PlayStation pad its shapes, in the keys alone */
    {
        static const char said[] = "D-pad: move. A: select. ~X/Y: A or B. LT/RT: page. B: back.";
        char out[128];
        gui_pad_keys_named(said, XBOX, out, sizeof out);
        assert(!strcmp(out, said));
        gui_pad_keys_named(said, SWITCH, out, sizeof out);
        assert(!strcmp(out, said));
        gui_pad_keys_named(said, PS, out, sizeof out);
        assert(!strcmp(out, "D-pad: move. Cross: select. ~Square/Triangle: A or B. LT/RT: page. Circle: back."));
        gui_pad_keys_named(said, PS, out, 20); /* cut short, ended */
        assert(strlen(out) == 19 && !strncmp(out, "D-pad: move. Cross:", 19));
        gui_pad_keys_named("LT/RT: page.", XBOX, out, 3); /* full right before a slash: it ends all the same */
        assert(!strcmp(out, "LT"));
        gui_pad_keys_named("no keys here", PS, out, sizeof out);
        assert(!strcmp(out, "no keys here"));
    }
    /* the shoulders go round the tabs, LB back with Shift; the arrows and the pages repeat, the others not */
    assert(gui_pad_key(SNX_PAD_LB, XBOX, &code, &mod, &repeats) && code == SDLK_TAB && (mod & SDL_KMOD_SHIFT));
    assert(!repeats);
    assert(gui_pad_key(SNX_PAD_RB, XBOX, &code, &mod, &repeats) && code == SDLK_TAB && !mod && !repeats);
    assert(gui_pad_key(SNX_PAD_DOWN, XBOX, &code, &mod, &repeats) && code == SDLK_DOWN && repeats);
    assert(gui_pad_key(SNX_PAD_RT, XBOX, &code, &mod, &repeats) && code == SDLK_PAGEDOWN && repeats);
    assert(gui_pad_key(SNX_PAD_SOUTH, XBOX, &code, &mod, &repeats) && !repeats);
    /* L3 and R3: Games' mods folder and AOT pack, once a press */
    assert(gui_pad_key(SNX_PAD_LS, XBOX, &code, &mod, &repeats) && code == SDLK_M && !repeats);
    assert(gui_pad_key(SNX_PAD_RS, XBOX, &code, &mod, &repeats) && code == SDLK_B && !repeats);
    /* the right stick pushed: Games' Play, once */
    assert(gui_pad_key(GUI_PAD_RIGHT_STICK, XBOX, &code, &mod, &repeats) && code == SDLK_L && !mod && !repeats);
    assert(!gui_pad_key(-1, XBOX, &code, &mod, &repeats));

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0"); /* the machine's pads through HIDAPI left out, as few as can be */
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "0");
    assert(SDL_Init(SDL_INIT_EVENTS));
    SDL_zero(p);
    /* a pad plugged into the machine opens too: the virtual ones are counted past those (their presses are the
       test's, the machine's not pressed meanwhile) */
    assert(gui_pads_start(&p));
    machine = p.count;
    xbox = plug(0x045e, 0x028e); /* an Xbox 360 pad's */
    pro = plug(0x057e, 0x2009);  /* a Switch Pro Controller's */
    assert(xbox && pro);
    pump(&p, 0, &key);
    if (p.count != machine + 2) {
        fprintf(stderr, "gui_pad: %d pads open, %d expected\n", p.count, machine + 2);
        return 1;
    }
    jx = SDL_GetJoystickFromID(xbox);
    jp = SDL_GetJoystickFromID(pro);
    assert(jx && jp);

    /* each face as its labels say */
    assert(tap(&p, jx, SDL_GAMEPAD_BUTTON_SOUTH) == SDLK_RETURN && tap(&p, jx, SDL_GAMEPAD_BUTTON_EAST) == SDLK_ESCAPE);
    assert(tap(&p, jp, SDL_GAMEPAD_BUTTON_EAST) == SDLK_RETURN && tap(&p, jp, SDL_GAMEPAD_BUTTON_SOUTH) == SDLK_ESCAPE);
    /* Home presses nothing: it is the games' menu's */
    SDL_SetJoystickVirtualButton(jx, SDL_GAMEPAD_BUTTON_GUIDE, true);
    assert(pump(&p, 0, &key) == 0);
    SDL_SetJoystickVirtualButton(jx, SDL_GAMEPAD_BUTTON_GUIDE, false);
    assert(pump(&p, 0, &key) == 0);

    /* Down held: pressed, again after the delay, then at the rate; let go, no more */
    SDL_SetJoystickVirtualButton(jx, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    assert(pump(&p, 1000, &key) == 1 && key.key.key == SDLK_DOWN && !key.key.repeat);
    assert(!gui_pads_repeat(&p, 1000 + GUI_PAD_DELAY_MS - 1, &key));
    assert(gui_pads_repeat(&p, 1000 + GUI_PAD_DELAY_MS, &key) && key.key.key == SDLK_DOWN && key.key.repeat);
    assert(!gui_pads_repeat(&p, 1000 + GUI_PAD_DELAY_MS + GUI_PAD_RATE_MS - 1, &key));
    assert(gui_pads_repeat(&p, 1000 + GUI_PAD_DELAY_MS + GUI_PAD_RATE_MS, &key) && key.key.key == SDLK_DOWN);
    SDL_SetJoystickVirtualButton(jx, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    assert(pump(&p, 2000, &key) == 0 && !gui_pads_repeat(&p, 5000, &key));

    /* the left stick: up once past the way, nothing more as it goes further; flicked down, Down; back, nothing */
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_LEFTY, -GUI_PAD_STICK - 1);
    assert(pump(&p, 3000, &key) == 1 && key.key.key == SDLK_UP);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    assert(pump(&p, 3000, &key) == 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    assert(pump(&p, 3000, &key) == 1 && key.key.key == SDLK_DOWN);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_LEFTY, 0);
    assert(pump(&p, 3000, &key) == 0 && !gui_pads_repeat(&p, 9000, &key));
    /* the right stick, any way past the left one's: L once, held or turned to the other axis; again once it was back */
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTX, GUI_PAD_STICK);
    assert(pump(&p, 3500, &key) == 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTX, GUI_PAD_STICK + 1);
    assert(pump(&p, 3500, &key) == 1 && key.key.key == SDLK_L && !key.key.repeat);
    assert(!gui_pads_repeat(&p, 3500 + GUI_PAD_DELAY_MS, &key));
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTY, -32768);
    assert(pump(&p, 3500, &key) == 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTX, 0); /* Y still pushed: the same push */
    assert(pump(&p, 3500, &key) == 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    assert(pump(&p, 3500, &key) == 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(jx, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    assert(pump(&p, 3500, &key) == 0);
    SDL_SetJoystickVirtualAxis(jp, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    assert(pump(&p, 3500, &key) == 1 && key.key.key == SDLK_L);
    SDL_SetJoystickVirtualAxis(jp, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    assert(pump(&p, 3500, &key) == 0 && !gui_pads_repeat(&p, 9000, &key));
    /* a trigger past its way is the page down, held as a button */
    SDL_SetJoystickVirtualAxis(jp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    assert(pump(&p, 4000, &key) == 1 && key.key.key == SDLK_PAGEDOWN);
    assert(gui_pads_repeat(&p, 4000 + GUI_PAD_DELAY_MS, &key) && key.key.key == SDLK_PAGEDOWN);
    /* let go: a virtual pad's trigger is its axis' whole range, so 0 would be half way */
    SDL_SetJoystickVirtualAxis(jp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    assert(pump(&p, 4000, &key) == 0 && !gui_pads_repeat(&p, 9000, &key));

    /* taken out: closed, its presses no longer read */
    SDL_DetachVirtualJoystick(pro);
    pump(&p, 0, &key);
    assert(p.count == machine + 1);
    gui_pads_stop(&p);
    assert(!p.started && !p.count);
    SDL_DetachVirtualJoystick(xbox);
    SDL_Quit();
    puts("gui_pad: ok");
    return 0;
}
