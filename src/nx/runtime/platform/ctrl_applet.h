/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The controller applet's dialog. A game that calls the controller applet says which controllers it expects: how many
   players, the styles it takes, how a single Joy-Con is held, and per player a text and a color of its own. The dialog
   explains that request first, then shows one card per player that the host's controllers fill, and its answer goes
   back to the game only when it closes (am.c holds the game's call meanwhile, as the console's applet does).

   This file has no SDL and no HLE state: the request read from the applet's arguments, the sentences that explain it,
   the dialog's model, its drawing in the on-screen layer (ui.h) and its input, and the result's bytes. The backend
   (backend/sdl3.c) tells it the host's controllers each poll and does what its actions ask. Its state is under
   ui_lock, as the overlay's */
#ifndef SNX_CTRL_APPLET_H
#define SNX_CTRL_APPLET_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/ui.h"
#include <stddef.h>

#define CTRL_PLAYERS 8 /* the most players an argument names */
#define CTRL_TEXT    0x81 /* a player's text in the argument, UTF-8 with its terminator */

/* what the game asks the applet to do */
enum { CTRL_MODE_SUPPORT, CTRL_MODE_STRAP_GUIDE, CTRL_MODE_FIRMWARE, CTRL_MODE_REMAP };
/* the styles, as the game's style set has them */
#define CTRL_STYLE_FULL_KEY  (1u << 0)
#define CTRL_STYLE_HANDHELD  (1u << 1)
#define CTRL_STYLE_JOY_DUAL  (1u << 2)
#define CTRL_STYLE_JOY_LEFT  (1u << 3)
#define CTRL_STYLE_JOY_RIGHT (1u << 4)
#define CTRL_STYLE_JOY_DUAL2 (1u << 12)
#define CTRL_STYLE_JOY_LEFT2 (1u << 13)
#define CTRL_STYLE_SINGLE    (CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT | CTRL_STYLE_JOY_LEFT2)
#define CTRL_STYLE_DUAL      (CTRL_STYLE_JOY_DUAL | CTRL_STYLE_JOY_DUAL2)

/* The game's request. The base comes from what the game told hid before the call (its styles and hold), and the
   applet's arguments go over it */
typedef struct {
    int mode;            /* CTRL_MODE_* */
    u32 version;         /* the applet's version in the common arguments, 0 when not given */
    int slots;           /* the players the argument's layout has room for: 4 or 8 */
    int min, max;        /* the players the game takes, 1 to slots */
    int take_over;       /* the controllers already connected stay */
    int left_justify;    /* the players fill from the first, with no gap */
    int permit_dual;     /* two Joy-Cons together count as one player */
    int single;          /* one player alone, whatever the counts say */
    u32 styles;          /* CTRL_STYLE_* the game takes */
    int hold;            /* how a single Joy-Con is held: 0 upright, 1 sideways */
    int has_colors, has_texts;
    u8 color[CTRL_PLAYERS][4];           /* each player's color, R G B A */
    char text[CTRL_PLAYERS][CTRL_TEXT];  /* each player's text, UTF-8, "" for none */
    /* the game takes only Joy-Con 2 styles, so it may play with the Joy-Con 2 as a mouse. Nothing it tells hid says
       whether it does: one game starts the mouse sensor for every player at setup, in scenes with no mouse too. So
       the dialog never says it does, and shows the mouse's check only as an optional panel, worded "if" */
    int mouse;
    /* the players whose controllers left, as "P1" or "P1, P3", when the dialog shows for them (the game paused for
       them, gamepad_assign=1) instead of for the game's call: "" for the game's call */
    char lost[24];
    u32 mouse_hands;     /* the hands whose mouse sensor the game started: bit 0 the left, bit 1 the right */
} CtrlRequest;

/* the request before the arguments: one player, the game's styles (0: all) and hold */
void ctrl_request_base(CtrlRequest* r, u32 styles, int hold);
/* the applet's arguments over the base: the common arguments, the private one and the argument itself, each NULL when
   the game did not push it. The layout comes from the sizes, the version only telling them apart where a size alone
   does not. 0 when the argument was read, -1 when nothing in it could be (the base stays) */
int ctrl_request_parse(CtrlRequest* r, const u8* common, u64 common_size, const u8* priv, u64 priv_size, const u8* arg,
                       u64 arg_size);
/* the cards the dialog shows: max, 1 in single mode */
int ctrl_request_cards(const CtrlRequest* r);
/* the request in words: the head ("This game needs 2 to 4 players.") and how each player plays ("Each player: one
   Joy-Con held sideways.") */
void ctrl_request_say(const CtrlRequest* r, char* head, size_t head_size, char* each, size_t each_size);
/* the style a host pad shows as, in words ("one Joy-Con held sideways") */
const char* ctrl_style_words(const CtrlRequest* r);
/* the kind a host pad must be given for the game to see it (a CTRL_PAD_*), -1 when it is seen as it is */
int ctrl_host_kind(const CtrlRequest* r);

/* the result the game reads back: the players, the Npad of the first, and whether the player cancelled */
#define CTRL_RESULT_SIZE 0xc
void ctrl_result_bytes(u8 out[CTRL_RESULT_SIZE], int players, u32 npad_id, int cancelled);

/* ---- the dialog */
/* a controller of the host's, by the player it holds (its gamepad slot): a generic pad (any style the game takes),
   Joy-Cons as a pair or alone, or a console Pro Controller (a Pro Controller to the game, nothing else). Joy-Cons and
   a Pro Controller keep their own styles, as the console shows them */
enum { CTRL_PAD_HOST, CTRL_PAD_JOY_PAIR, CTRL_PAD_JOY_LEFT, CTRL_PAD_JOY_RIGHT, CTRL_PAD_PRO };
typedef struct {
    int present;      /* a controller holds this player */
    int kind;         /* CTRL_PAD_*: a host pad shows as whatever style the game takes */
    int joycon2;      /* a Joy-Con 2 */
    int keyboard;     /* no controller: player 1 is the keyboard's */
    /* joycon2: one the runtime connects itself over Bluetooth (a Joy-Con 2 or a Pro Controller 2), which the dialog
       can disconnect */
    char name[48];
    int lost;         /* no controller: its controller left, and a pause waits for it (gamepad_assign=1) */
    /* as hid lays it out: a pair split (1 + the player its other half plays as) keeping its right half or its left
       one, and two singles, a left and a right, merged into one pair on the lower player (1 + the other one's) */
    int split, keep_right, merged;
} CtrlPad;
/* a Joy-Con 2 the scan heard while the dialog shows, not yet a controller */
typedef struct {
    int right;   /* the right one */
    int syncing; /* its sync button is held */
    int known;   /* the runtime took it before: it connects on its own */
    int asked;   /* new, and the user confirmed it: it is taken */
    int pro;     /* a Pro Controller 2, not a Joy-Con 2 */
    int held;    /* known, but held off since the dialog disconnected it: only its Connect item takes it back */
} CtrlFound;
/* a controller the scan heard, in words: "Joy-Con 2 (L)", "Joy-Con 2 (R)" or "Pro Controller 2" */
const char* ctrl_found_name(const CtrlFound* f);
#define CTRL_FOUND 8
/* the mouse case's live check: each hand on the table, the cursor's motion since the last poll, R and ZR held, and
   the right Joy-Con 2's reports per second (0 when not known) */
typedef struct {
    int real[2], surface[2];
    float dx, dy;
    int r, zr;
    int rate;
} CtrlMouse;

/* the dialog for r, in place of one that shows. The ticket is the asker's, handed back with the answer */
void ctrl_dialog_show(const CtrlRequest* r, u32 ticket);
void ctrl_dialog_hide(u32 ticket); /* taken away unanswered, as the game's applet ended */
int ctrl_dialog_shown(void);       /* any thread */
const CtrlRequest* ctrl_dialog_request(void);
/* each poll: the host's controllers by player, the Joy-Con 2 the scan hears (n of them) and the mouse's check */
void ctrl_dialog_pads(const CtrlPad pads[CTRL_PLAYERS]);
void ctrl_dialog_found(const CtrlFound* found, int n);
void ctrl_dialog_mouse(const CtrlMouse* m);
/* whether player 1 can be the Joy-Cons attached to the console (the TV mode off), and whether it is now: a game that
   doesn't take the handheld controls, and the mouse, need them detached, which an item does (CTRL_DO_DETACH) */
void ctrl_dialog_attached(int can, int attached);
/* whether the PC mouse reaches the game as the right mouse now, as its item shows it */
void ctrl_dialog_pc_mouse(int on);
/* a problem to say under the cards ("Bluetooth is off"), "" for none */
void ctrl_dialog_problem(const char* text);
/* whether a card is ready: its controller fits the request */
int ctrl_dialog_ready(int card);
/* the players ready among the cards, and the first of them (-1 for none) */
int ctrl_dialog_ready_count(int* first);

/* What an input asks the backend to do: move the controller of player from to player to (a swap), the dialog
   answered, identify a player's controller, the PC mouse as the right mouse on or off, a new Joy-Con 2 (found index)
   taken. And what the console's gestures ask, each first moving the controller from to the free player card when it
   is not -1:
   - SINGLE: SL and SR on a single Joy-Con, which leaves a pair it was merged into and plays alone, held sideways.
   - SPLIT: SL and SR on one half of a pair (side 1 the right one), which plays alone, the other half on a free player.
   - MERGE: L on a left single and R on a right single held together (from the left one's player, to the right one's),
     one pair on the lower player of the two (card: the left one moved first when neither has a card).
   - WHOLE: L and R together on a pair or a pad, which plays whole (a split pair joined again) */
enum {
    CTRL_DO_NONE,
    CTRL_DO_MOVE,
    CTRL_DO_ANSWERED,
    CTRL_DO_IDENTIFY,
    CTRL_DO_PC_MOUSE,
    CTRL_DO_TAKE,
    CTRL_DO_SINGLE,
    CTRL_DO_SPLIT,
    CTRL_DO_MERGE,
    CTRL_DO_WHOLE,
    CTRL_DO_DETACH,    /* from: 1 the Joy-Cons detached from the console (tabletop), 0 attached again */
    CTRL_DO_DISCONNECT /* from: the player whose controller (one the runtime connected itself) is disconnected */
};
typedef struct {
    int what;
    int from, to;
    int card, side;
} CtrlDo;
/* every player's controller each poll: the buttons pressed since the last poll (the game's button bits, a pair's SL
   and SR too) and the ones held. A works the items, B cancels, X identifies, Up and Down move the focus. A controller
   joins only by the console's gestures: SL and SR together on one Joy-Con (alone, sideways), L on a left Joy-Con and
   R on a right one held together (a pair), L and R together on a pair or any other pad (whole). A gesture whose
   style the game does not take is refused, its reason said in the card */
CtrlDo ctrl_dialog_press(int player, u64 pressed, u64 held);
/* the keyboard: a move of the focus, Enter, Esc */
enum { CTRL_KEY_UP, CTRL_KEY_DOWN, CTRL_KEY_ENTER, CTRL_KEY_ESC };
CtrlDo ctrl_dialog_key(int key);
/* a tap or a click at x, y on a picture sw x sh, the letters at scale */
CtrlDo ctrl_dialog_tap(float x, float y, int sw, int sh, int scale);
/* the answer, taken once: 0 continued or 1 cancelled, with its ticket, and -1 for none */
int ctrl_dialog_take(u32* ticket);

/* the dialog into l on a picture sw x sh, its letters at scale at most (smaller when it would not fit) */
void ctrl_dialog_draw(SnxUiList* l, int sw, int sh, int scale);
/* counts every change of what the dialog draws, for a drawing kept from frame to frame */
u32 ctrl_dialog_version(void);
#endif
