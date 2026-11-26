/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls: a pad drawn over the game (ui.h) that fingers or the mouse press, with rEden's touch
   overlay's sprites, sizes and options, laid out as the console's two controllers hold them. The same on every
   platform: Auto by default, shown on a touch screen while no gamepad is connected. Kept in the game's .ini (keys
   controls and controls_*) */
#ifndef SNX_VPAD_H
#define SNX_VPAD_H
#include "common/utilities/ui_quads.h"
#include "snx_settings.h"

/* each control's SNX_VPAD_ name and the id of its keys (controls_<id>, controls_<id>_shown), in the order they are drawn:
   a finger presses the last one drawn under it */
#define SNX_VPAD_IDS(ROW)                                                                                              \
    ROW(L, "l")                                                                                                        \
    ROW(R, "r")                                                                                                        \
    ROW(ZL, "zl")                                                                                                      \
    ROW(ZR, "zr")                                                                                                      \
    ROW(MINUS, "minus")                                                                                                \
    ROW(PLUS, "plus")                                                                                                  \
    ROW(CAPTURE, "capture")                                                                                            \
    ROW(HOME, "home")                                                                                                  \
    ROW(L3, "l3")                                                                                                      \
    ROW(R3, "r3")                                                                                                      \
    ROW(DPAD, "dpad")                                                                                                  \
    ROW(STICK_L, "stick_l")                                                                                            \
    ROW(STICK_R, "stick_r")                                                                                            \
    ROW(A, "a")                                                                                                        \
    ROW(B, "b")                                                                                                        \
    ROW(X, "x")                                                                                                        \
    ROW(Y, "y")
#define SNX_VPAD_INDEX(name, id) SNX_VPAD_##name,
enum { SNX_VPAD_IDS(SNX_VPAD_INDEX) SNX_VPAD_COUNT };
/* what the frontend does, read with vpad_actions */
#define SNX_VPAD_DO_MENU 1u /* Home was pressed: the game's menu */
#define SNX_VPAD_DO_SHOT 2u /* Capture was pressed: a screenshot */
#define SNX_VPAD_DO_BUZZ 4u /* a control went down or up: a short buzz, haptics being on */

typedef struct {
    float x, y; /* its middle, 0 to 1 across the window's clear part */
    int size;   /* percent of its own default size */
} SnxVpadPlace;
/* the controls' layout, read from their places (controls_a...). Their other settings are g_settings' (settings.txt):
   show_controls, controls_scale (rEden's 0 to 100: every control at (scale + 50)%), snap_to_grid,
   relative_stick_center, d_pad_slide, haptics, and control_shown[SNX_VPAD_] */
typedef struct {
    SnxVpadPlace at[SNX_VPAD_COUNT];
} SnxVpadConfig;
/* A console's own pad: its controls, each the Switch's where it sits (its .ini key, its shape, its size and its button
   bit), with the console's name and label for it; a control it has none of is not there, and each is whole in the
   window. In a window wider than tall they start where the Switch's do, so whoever knows the Switch's pad finds each in
   the same place, but where the console puts one elsewhere (wide, x 0 for the Switch's place); in one taller than wide,
   at the console's own place for it, under its picture (tall). touch: it has a touch screen */
typedef struct {
    int control; /* SNX_VPAD_ */
    const char *name, *label;
    struct {
        float x, y;
    } wide, tall;
} SnxVpadControl;
typedef struct {
    const char* name; /* the console's, as a line says it: "GBA" */
    const SnxVpadControl* pad;
    int count, touch;
} SnxVpadConsole;
/* the pad is console's from now on, its layout the default: before its settings are read. The Switch's until then */
void vpad_console(const SnxVpadConsole* console);
/* the layout, its defaults filled in on first use */
SnxVpadConfig* vpad_config(void);
#define SNX_VPAD_SIZE_MIN 50
#define SNX_VPAD_SIZE_MAX 200

void vpad_defaults(SnxVpadConfig* c); /* the default layout */
/* a setting the controls show changed outside vpad.c's own calls (the menu's buttons page): drawn anew. vpad.c's own
   calls that change what they draw count it themselves */
void vpad_changed(void);
/* controls=2 (Auto): whether the controls show on a device with a touch screen (touch) and pads gamepads connected:
   while it has one and none is connected */
int vpad_auto_shown(int touch, int pads);
/* the log's line on what Auto shows and why, into out; snprintf's count */
int vpad_auto_said(int touch, int pads, char* out, size_t n);
/* the device's touch screen and gamepads, as the game starts and as a gamepad comes or goes: what controls=2 shows,
   said in the log as it changes */
void vpad_auto(int touch, int pads);
const char* vpad_name(int control); /* a control's name, as edit mode shows it: "A", "D-pad" */
const char* vpad_console_name(void); /* the console vpad_console gave ("GBA"), "Switch" before it */
/* the length a control's size is a share of, for the clear part safe (x, y, w, h): its shorter side */
float vpad_side(const int safe[4]);
/* edit mode's bar over safe as vpad_draw last laid it out (its letters' scale), x, y, w, h into box: at the top, or
   under a control in its way; whether edit mode is on */
int vpad_edit_bar(const int safe[4], float box[4]);
/* the window's console has control (every one for the Switch's) */
int vpad_has(int control);
/* control's rectangle in safe, x, y, w, h into r: its place's middle, its size, whole in the window */
void vpad_box(int control, const int safe[4], float r[4]);
/* places, sizes and the shown ones back to their defaults, kept */
void vpad_reset_layout(void);
/* the settings' tables (settings.h): g_vpad_ini, controls, controls_scale..., a control's place (controls_a); and
   g_vpad_shown_ini, a control shown or not (controls_a_shown), in the order of SNX_VPAD_ */
/* the game's menu's page of these settings and its page of the buttons shown, as the launcher's Settings name them */
#define SNX_VPAD_PAGE    "On-screen controls"
#define SNX_VPAD_BUTTONS "Buttons"
/* a key of the game's .ini (controls, controls_scale..., controls_a, controls_a_shown...): applied, 1; not one of
   these, 0 */
int vpad_ini(const char* key, const char* value);
/* Every console's but the Switch's, after vpad_console. A command line's argument: 1 for --KEY=VALUE of a setting of
   the controls or of touch, or --launcher-picks=0|1, which vpad_settings then reads; 0 for another; -1 for a value the
   setting does not take, why in error */
int vpad_arg(const char* arg, char* error, size_t n);
/* another table's settings that vpad_arg and vpad_settings take with theirs, as they take theirs: find gives a key's
   row, NULL for a key of none (the runtime's keys', hotkey_setting). Before the first vpad_arg */
void vpad_settings_also(const IniOpt* (*find)(const char* key));
/* The settings of them the game name in folder has: its .ini's (<folder>/<name>.ini, where its driver keeps it), then
   its own picks (game_ini.h's snx_game_picks) and the command line's, which go over them, or with --launcher-picks=1
   under them (the launcher's picks for every game). One the console has nothing for does nothing, and is said with its
   value */
void vpad_settings(const char* folder, const char* name, int argc, char** argv);
/* the rows of the controls and of what reads with them (vpad_settings_also) in the settings file at path, the last line
   of a key counting; the file's other keys, its game's other settings, left to their reader. One that is there but
   cannot be read is said, and nothing of it is in effect */
void vpad_settings_file(const char* path);
/* where changed settings go: called with each key and its value (the game's .ini) */
void vpad_set_store(void (*store)(const char* key, const char* value));
/* where what the controls say goes, a line without its newline (vpad_auto's): the game's log */
void vpad_set_say(void (*say)(const char* line));
/* called as the touch screen's setting is read (g_settings.touch_screen) */
void vpad_set_touch(void (*changed)(void));
/* a setting's current value to the store, named by its variable (&g_settings.controls_scale); a control's place
   with vpad_store_control, whether it is shown with vpad_store_shown */
void vpad_store(const int* setting);
void vpad_store_opt(const IniOpt* o); /* any table's setting, as its variable holds it */
void vpad_store_control(int control);
void vpad_store_shown(int control);

/* Fingers, and the mouse as one, in window pixels; safe is the window's clear part (x, y, w, h). A finger that lands
   on a control, or any in edit mode, is the controls': 1, and its moves and lift are theirs */
int vpad_down(u64 id, float x, float y, const int safe[4]);
void vpad_move(u64 id, float x, float y, const int safe[4]);
int vpad_up(u64 id);
void vpad_release(void); /* every finger off: the menu opened, the window went away */
/* the game's menu opened (1) or closed (0): while it is open the controls neither show nor take a finger, so the
   two do not lie over each other, and every finger is let go as it opens */
void vpad_menu(int open);
/* the controls' buttons as HidNpadButton bits, and their sticks (lx, ly, rx, ry) from -32767 to 32767, right and up;
   a button pressed and let go since the last call is in the bits this once */
void vpad_state(u64* buttons, i32 sticks[4]);
unsigned vpad_actions(void); /* SNX_VPAD_DO_ bits since the last call */

/* edit mode: a control dragged moves, one tapped can be sized from the bar on top, Done (or vpad_edit(0)) ends it */
void vpad_edit(int on);
int vpad_editing(void);
int vpad_shown(void); /* drawn: on, or in edit mode */

/* safe in the pixels of the picture drawn on; scale the menu's letters' (common/utilities/notice.h's snx_menu_scale),
   which edit mode's bar is written in, and taken where it was last drawn. What it did: the quads of the draw before
   appended as they were, laid out again, or nothing, the controls hidden */
enum { VPAD_DRAW_KEPT, VPAD_DRAW_LAID, VPAD_DRAW_HIDDEN };
int vpad_draw(SnxUiList* l, const int safe[4], int scale);
/* one control's look in the rectangle r (x, y, w, h), of its aspect (its width over its height) where the caller keeps
   its shape: what buttons (HidNpadButton bits) hold drawn pressed, a stick's knob at axis (-1 to 1, right and down) */
float vpad_aspect(int control);
void vpad_control_draw(SnxUiList* l, int control, const float r[4], u64 buttons, const float axis[2]);

#endif
