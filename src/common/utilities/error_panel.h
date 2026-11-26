/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel: one screen for an error that stops a game, the same in every console's window, in the panel's
   own process and in the launcher. Laid out as the game's menu lays out its dialogs (the 8x16 font, the YoRHa colours,
   the menu's scale), as quads (ui_quads.h) of fills and letters alone, so a window with no GPU draws them as rectangles
   (ui_rects.h).

   Its main page: the head, the game's name, the error's line (error_kind.h) and what to do, the actions, and a line of
   the keys that work. Details, its second page: the error as the log says it, scrolled where it is long, the log's
   place, Open Log, Copy Text and Back. Up and Down move between the items, the one that fixes the error in focus
   first; do is Enter, a pad's A or a tap on an item; Esc, a pad's B and Android's Back close it (on Details, back to
   the main page); nothing else does anything. No input counts until SNX_ERROR_PANEL_WAIT_MS after it showed, so a
   press meant for the game does not take an action. What the actions do is the caller's: the panel says which */
#ifndef SNX_ERROR_PANEL_H
#define SNX_ERROR_PANEL_H
#include "common/utilities/error_kind.h"
#include "common/utilities/ui_quads.h"

#define SNX_ERROR_PANEL_WAIT_MS 500
#define SNX_ERROR_PANEL_QUADS   4096 /* a panel's most: its letters and fills, a long message's rows on a wide window */
#define SNX_ERROR_PANEL_ITEMS   SNX_ERROR_ACTIONS_MAX

typedef struct {
    float x, y, w, h;
} SnxErrorRect;
/* the input used last, whose keys the panel's foot names */
enum { SNX_ERROR_INPUT_KEYS, SNX_ERROR_INPUT_PAD, SNX_ERROR_INPUT_TOUCH };
/* a key, a pad's button or a direction, as the panel takes it */
enum {
    SNX_ERROR_KEY_UP,
    SNX_ERROR_KEY_DOWN,
    SNX_ERROR_KEY_DO,
    SNX_ERROR_KEY_BACK,
    SNX_ERROR_KEY_PAGE_UP,
    SNX_ERROR_KEY_PAGE_DOWN
};

typedef struct {
    u64 shown_ms; /* when it showed, on the caller's clock */
    int details;  /* on the Details page */
    int focus;    /* the item in focus, an index into the page's items */
    int top;      /* Details: the message's first row shown */
    int input;    /* SNX_ERROR_INPUT_ */
    /* the screen: its display's scale in percent (SDL's display scale times 100), rows a finger high (a phone's), its
       Back key (Android's), and whether Open Log can open a log there (a desktop's viewer) */
    int percent, fingers, back_key, open_log;
    char pad_do[16], pad_back[16]; /* a pad's names for do and back ("A", "B"; "Cross", "Circle") */
    char said[512];                /* what the last action did ("Log saved to ~/x.log"), over the items: "" none */
    /* drawn last: the box, each item's rows and its action, how many items, the message's rows and how many show */
    SnxErrorRect box, item[SNX_ERROR_PANEL_ITEMS];
    int action[SNX_ERROR_PANEL_ITEMS], items, rows, rows_shown;
    int pressed; /* the item a press went down on, -1 for none */
    int at_once; /* no wait: what it follows was no game's press (the launcher's own failure), set after the start */
} SnxErrorPanel;

/* the panel at its start, shown at now_ms, the action that fixes e in focus */
void snx_error_panel_start(SnxErrorPanel* p, u64 now_ms);
/* its quads for a window of sw by sh pixels into l, e's text in them; where each item is kept in p for the hits */
void snx_error_panel_draw(SnxErrorPanel* p, const SnxError* e, SnxUiList* l, int sw, int sh);
/* key (SNX_ERROR_KEY_) from input (SNX_ERROR_INPUT_) at now_ms: the action to do (SNX_ACTION_), or NONE. The panel's
   own (Details and Back, which change its page) are done here and give NONE; CLOSE from the main page is the caller's */
int snx_error_panel_key(SnxErrorPanel* p, const SnxError* e, int key, int input, u64 now_ms);
/* a finger or the mouse's left button down (down 1) or up at x, y in the window's pixels: the action of the item it
   went down and came up on, else NONE. touch: a finger's, whose keys the foot then names */
int snx_error_panel_press(SnxErrorPanel* p, const SnxError* e, float x, float y, int down, int touch, u64 now_ms);
/* the message on Details moved rows down (up when below 0), within its ends */
void snx_error_panel_scroll(SnxErrorPanel* p, int rows);
#endif
