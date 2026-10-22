/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button, a gamepad's Home, the on-screen Home or F2 opens it over the game, in the
   menu's look (src/gui, the YoRHa theme), drawn by the renderer as quads (ui.h). The same on every platform: the
   on-screen controls' options (vpad.h), the touch screen's, and Exit */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "nx/runtime/platform/ui.h"

#define OVERLAY_COLS     40 /* the panel, in cells of the 8x16 font: the narrowest */
#define OVERLAY_COLS_MAX 72 /* and the widest, as far as three quarters of the picture let it */
#define OVERLAY_ROWS     16 /* the tallest page's: the others are as tall as their items */
/* what a menu action leaves to its caller: the save state ones for the slot overlay_slot names */
enum { OVERLAY_NONE, OVERLAY_CLOSE, OVERLAY_EXIT, OVERLAY_EDIT, OVERLAY_SAVE, OVERLAY_LOAD, OVERLAY_DELETE };

/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* the display's own scale (SDL's: dpi / 160 on a phone), which the panel's letters follow; 1 until told */
void overlay_set_display_scale(float scale);
int overlay_display_percent(void); /* that scale, in percent */
/* where the panel goes on a picture of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
int overlay_select(void);      /* the focused item's action: a page, a setting changed, or an OVERLAY_ for the caller */
void overlay_move(int rows);   /* the focus up (-1) or down (1), round the page */
void overlay_scroll(int rows); /* the mouse's wheel: the same, stopping at the ends */
void overlay_side(int step);   /* the focused setting down (-1) or up (1) */
int overlay_back(void);        /* the page above; OVERLAY_CLOSE from the first */
/* a button pressed, as the game reads it (one HidNpadButton bit): the D-pad and the left stick move, A selects, B goes
   back, the rest do nothing. What it did, as overlay_select */
int overlay_pad(u64 button);
/* the pads each poll, the on-screen controls and the connected ones: their buttons and sticks at ms. With the menu
   shown, each button pressed since the last call acts as overlay_pad, the left stick past half way as the D-pad; a
   direction held goes on after SNX_OVERLAY_REPEAT_DELAY ms, every SNX_OVERLAY_REPEAT_MS; a press held as the menu opens
   does not act. What the last one did, or the first that leaves the menu */
#define SNX_OVERLAY_REPEAT_DELAY 400
#define SNX_OVERLAY_REPEAT_MS    120
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms);
/* the button map page waits for a control pressed on a pad (pad_map.h's): it takes it, and that press is no menu
   key */
int overlay_listening(void);
void overlay_press(int control);
/* a tap at x, y on a picture of sw x sh: what it did, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
/* a finger or the mouse (touch.h's SNX_TOUCH_MOUSE) on the open menu, at x, y in the picture's pixels: down on an
   on-screen control it is the controls', whose presses move the menu (overlay_controls); else its lift taps the menu
   where it lifts (what that did). Reset forgets the one down, which lifts without a tap; cancel forgets id's */
void overlay_pointer_down(u64 id, float x, float y, const int safe[4]);
void overlay_pointer_move(u64 id, float x, float y, const int safe[4]);
int overlay_pointer_up(u64 id, float x, float y, int sw, int sh);
void overlay_pointer_cancel(u64 id);
void overlay_pointers_reset(void);
/* the menu, and over the game a notification until its time is up, at now_ns (plat_ns) */
void overlay_draw(SnxUiList* l, int sw, int sh, u64 now_ns);
/* a notification of one line in the picture's corner for SNX_OVERLAY_TOAST_MS from now_ns: a save state saved or
   loaded, a screenshot taken. Under ui_lock */
#define SNX_OVERLAY_TOAST_MS 3000
void overlay_toast(const char* text, u64 now_ns);
int overlay_toasting(u64 now_ns); /* one shows */
/* The save states' pages: a row per slot, the focused one's thumbnail beside them, and Save, Load and Delete for one
   (Load and Delete asked first). Slot 0 is the one a load saves over first, which only loads or goes */
#define OVERLAY_SLOTS_MAX 33
typedef struct {
    char when[32]; /* when it was saved, and the play time then; "" for an empty slot */
    char why[96];  /* why it can't be loaded, "" when it can */
    u8* thumb;     /* SNX_UI_IMAGE_W x SNX_UI_IMAGE_H RGBA, NULL for none: allocated, the overlay's to free */
} OverlaySlot;
/* the slots listed, n of them from slot 0, taken over, under ui_lock: the frontend's as each page opens and after a
   save or a delete */
void overlay_slots_set(const OverlaySlot* slots, int n);
int overlay_slot(void);        /* the slot the last OVERLAY_SAVE, _LOAD or _DELETE is for */
int overlay_states_shown(void); /* one of the save states' pages shows: the game waits meanwhile */
int overlay_states_stale(void); /* the slots page opened since the last overlay_slots_set: its list is read again */
/* the columns the menu's pages and the stopped panel take at scale on a picture sw wide: as many as three quarters of it
   holds, within OVERLAY_COLS and OVERLAY_COLS_MAX */
int overlay_cols(int sw, int scale);
/* a fatal error's panel (backend/sdl3.c), a dialog as the menu's pages are: the head, the game's name, the error, what it
   means and what to do, cut to its width, then Share Log, Copy path and Close, one in focus */
typedef struct {
    float x, y, w, h;
} SnxOverlayRect;
enum { SNX_OVERLAY_ERROR_SHARE, SNX_OVERLAY_ERROR_COPY, SNX_OVERLAY_ERROR_CLOSE, SNX_OVERLAY_ERROR_ITEMS };
typedef struct {
    const char *name, *msg, *how;
    const char* log;  /* the game's log, underlined as a link; NULL for none */
    const char* said; /* what Share Log or Copy path did, over the items; NULL before either */
    int focus;        /* one of SNX_OVERLAY_ERROR_SHARE, _COPY and _CLOSE */
    /* drawn: where the log's link and each item are, empty when not shown */
    SnxOverlayRect link, items[SNX_OVERLAY_ERROR_ITEMS];
} SnxOverlayError;
void overlay_error_draw(SnxUiList* l, int sw, int sh, SnxOverlayError* e);
/* 1 when x, y falls in r, an empty one taking nothing */
int overlay_error_hit(const SnxOverlayRect* r, float x, float y);

#endif
