/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ctrl_applet.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "common/utilities/ui_colors.h"
#include "common/utilities/bits.h"
#include "gui/icons.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ============================================================ the dialog */
enum { WAITING, CONTINUED, CANCELLED };
/* ITEM_DROP + c: disconnect player c's controller, ITEM_TAKE + i: connect the controller the scan found at i */
enum { ITEM_CONTINUE, ITEM_CANCEL, ITEM_PC_MOUSE, ITEM_DETACH, ITEM_DROP, ITEM_TAKE = ITEM_DROP + CTRL_PLAYERS };
#define ITEMS (ITEM_TAKE + CTRL_FOUND)
static struct {
    _Atomic int shown;
    CtrlRequest req;
    u32 ticket;
    CtrlPad pad[CTRL_PLAYERS];
    CtrlFound found[CTRL_FOUND];
    int nfound;
    CtrlMouse mouse;
    float cursor[2]; /* the live check's cursor, 0 to 1 across its area */
    int pc_mouse;
    int can_attach, attached; /* player 1 can be the Joy-Cons attached to the console, and is */
    int focus; /* an ITEM_ */
    int answer;
    int refused; /* Continue pressed short of the least: said until a card changes */
    char problem[160], said[96];
    u64 held[CTRL_PLAYERS]; /* each player's buttons held at the last poll, for L and R across two Joy-Cons */
    int refused_card;       /* the card a refused gesture's reason shows in, -1 none */
    char refusal[128];
    _Atomic u32 version;
} g_ctrl = {.refused_card = -1};

static void changed(void) { atomic_fetch_add_explicit(&g_ctrl.version, 1, memory_order_release); }
u32 ctrl_dialog_version(void) { return atomic_load_explicit(&g_ctrl.version, memory_order_acquire); }

void ctrl_dialog_show(const CtrlRequest* r, u32 ticket) {
    g_ctrl.req = *r;
    g_ctrl.ticket = ticket;
    g_ctrl.focus = ITEM_CONTINUE;
    g_ctrl.answer = WAITING;
    g_ctrl.refused = 0;
    g_ctrl.problem[0] = g_ctrl.said[0] = 0;
    g_ctrl.refused_card = -1;
    memset(g_ctrl.held, 0, sizeof g_ctrl.held);
    g_ctrl.nfound = 0;
    g_ctrl.cursor[0] = g_ctrl.cursor[1] = 0.5f;
    atomic_store_explicit(&g_ctrl.shown, 1, memory_order_release);
    changed();
}
void ctrl_dialog_hide(u32 ticket) {
    if (g_ctrl.ticket == ticket) atomic_store_explicit(&g_ctrl.shown, 0, memory_order_release);
    changed();
}
int ctrl_dialog_shown(void) { return atomic_load_explicit(&g_ctrl.shown, memory_order_acquire); }
const CtrlRequest* ctrl_dialog_request(void) { return &g_ctrl.req; }
void ctrl_dialog_pads(const CtrlPad pads[CTRL_PLAYERS]) {
    if (memcmp(g_ctrl.pad, pads, sizeof g_ctrl.pad)) {
        memcpy(g_ctrl.pad, pads, sizeof g_ctrl.pad);
        g_ctrl.refused = 0;
        g_ctrl.refused_card = -1;
        changed();
    }
}
void ctrl_dialog_found(const CtrlFound* found, int n) {
    n = snx_clamp_int(n, 0, CTRL_FOUND);
    if (n != g_ctrl.nfound || memcmp(g_ctrl.found, found, sizeof *found * (size_t)n)) {
        memcpy(g_ctrl.found, found, sizeof *found * (size_t)n);
        g_ctrl.nfound = n;
        if (g_ctrl.focus >= ITEM_TAKE) g_ctrl.focus = ITEM_CONTINUE;
        changed();
    }
}
void ctrl_dialog_mouse(const CtrlMouse* m) {
    /* the cursor wanders over its area as the sensor reports, a count of 400 across it */
    g_ctrl.cursor[0] = snx_clamp_float(g_ctrl.cursor[0] + m->dx / 400.0f, 0.0f, 1.0f);
    g_ctrl.cursor[1] = snx_clamp_float(g_ctrl.cursor[1] + m->dy / 400.0f, 0.0f, 1.0f);
    g_ctrl.mouse = *m;
    changed();
}
void ctrl_dialog_attached(int can, int attached) {
    if (g_ctrl.can_attach != (can != 0) || g_ctrl.attached != (attached != 0)) {
        g_ctrl.can_attach = can != 0;
        g_ctrl.attached = attached != 0;
        changed();
    }
}
void ctrl_dialog_pc_mouse(int on) {
    if (g_ctrl.pc_mouse != (on != 0)) {
        g_ctrl.pc_mouse = on != 0;
        changed();
    }
}
void ctrl_dialog_problem(const char* text) {
    if (strcmp(g_ctrl.problem, text)) {
        snprintf(g_ctrl.problem, sizeof g_ctrl.problem, "%s", text);
        changed();
    }
}

/* whether the game takes a controller of kind (CTRL_PAD_*) as hid shows it. A generic pad shows as any style the game
   takes. A console controller keeps its own: a Pro Controller only as one, a pair as two Joy-Cons or two Joy-Con 2, a
   single as its own side. NULL when it does, else why in the game's terms */
static const char* misfit_kind(const CtrlRequest* r, int kind, int merged) {
    static char why[160];
    u32 ok = r->styles;
    (void)merged;
    switch (kind) {
    case CTRL_PAD_HOST: return NULL;
    case CTRL_PAD_PRO:
        if (ok & CTRL_STYLE_FULL_KEY) return NULL;
        snprintf(why, sizeof why, "this game takes only %s: the Pro Controller does not fit", ctrl_style_words(r));
        return why;
    case CTRL_PAD_JOY_PAIR:
        return ok & CTRL_STYLE_DUAL ? NULL : "this game does not accept both Joy-Cons as one player";
    case CTRL_PAD_JOY_LEFT:
        if (ok & (CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_LEFT2)) return NULL;
        break;
    case CTRL_PAD_JOY_RIGHT:
        if (ok & CTRL_STYLE_JOY_RIGHT) return NULL;
        break;
    default: return NULL;
    }
    return ok & CTRL_STYLE_SINGLE ? "this game does not accept that Joy-Con alone"
                                  : "this game does not accept a single Joy-Con";
}
/* whether the game takes one Joy-Con of side (1 the right one) alone, and two Joy-Cons as one pair */
static int single_ok(int right) {
    u32 ok = g_ctrl.req.styles;
    return right ? (ok & CTRL_STYLE_JOY_RIGHT) != 0 : (ok & (CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_LEFT2)) != 0;
}
static int pair_ok(int merged) { /* a console pair shows only as two Joy-Cons or two Joy-Con 2, merged or not */
    (void)merged;
    return (g_ctrl.req.styles & CTRL_STYLE_DUAL) != 0;
}
static int cards(void) { return ctrl_request_cards(&g_ctrl.req); }
/* whether player c's Joy-Con plays in another player's pair */
static int merged_into(int c) {
    for (int q = 0; q < CTRL_PLAYERS; ++q)
        if (g_ctrl.pad[q].present && g_ctrl.pad[q].merged == c + 1) return 1;
    return 0;
}
/* what plays card c's Npad as hid lays the players out: its own controller (whole, or the half its split keeps, or
   the pair its merge makes), another player's split half, or player 1's keyboard. None when its Joy-Con plays in
   another player's pair. The kind the game sees there and its name */
typedef struct {
    int held, kind, merged;
    char name[100];
} Holder;
static Holder holder(int c) {
    Holder h = {0, CTRL_PAD_HOST, 0, ""};
    const CtrlPad* p = &g_ctrl.pad[c];
    int q;
    for (q = 0; q < CTRL_PLAYERS; ++q) {
        const CtrlPad* o = &g_ctrl.pad[q];
        if (o->present && o->merged == c + 1) return h; /* its Joy-Con plays in q's pair */
        if (o->present && o->split == c + 1) {           /* q's other half */
            h.held = 1;
            h.kind = o->keep_right ? CTRL_PAD_JOY_LEFT : CTRL_PAD_JOY_RIGHT;
            snprintf(h.name, sizeof h.name, "%s, %s half", o->name, o->keep_right ? "left" : "right");
            return h;
        }
    }
    if (p->present) {
        h.held = 1;
        h.kind = p->kind;
        snprintf(h.name, sizeof h.name, "%s", p->name);
        if (p->kind == CTRL_PAD_JOY_PAIR && p->split) {
            h.kind = p->keep_right ? CTRL_PAD_JOY_RIGHT : CTRL_PAD_JOY_LEFT;
            snprintf(h.name, sizeof h.name, "%s, %s half", p->name, p->keep_right ? "right" : "left");
        } else if (p->merged > 0 && p->merged <= CTRL_PLAYERS) {
            h.kind = CTRL_PAD_JOY_PAIR;
            h.merged = 1;
            const CtrlPad* o = &g_ctrl.pad[p->merged - 1]; /* the left one's name first */
            snprintf(h.name, sizeof h.name, "%s + %s", p->kind == CTRL_PAD_JOY_LEFT ? p->name : o->name,
                     p->kind == CTRL_PAD_JOY_LEFT ? o->name : p->name);
        }
    } else if (c == 0 && p->keyboard && !p->lost) {
        h.held = 1;
        snprintf(h.name, sizeof h.name, "the keyboard");
    }
    return h;
}
/* why player 1 can't be the Joy-Cons attached to the console here, NULL when it can: the game doesn't take the
   handheld controls, or it plays with the mouse */
static const char* attach_refused(void) {
    if (!g_ctrl.attached || g_ctrl.req.mode != CTRL_MODE_SUPPORT || g_ctrl.req.lost[0]) return NULL;
    if (!(g_ctrl.req.styles & CTRL_STYLE_HANDHELD))
        return "This game does not take the Joy-Cons attached to the console: pick Detach below.";
    return NULL;
}
int ctrl_dialog_ready(int c) {
    Holder h;
    if (c < 0 || c >= cards()) return 0;
    h = holder(c);
    return h.held && !misfit_kind(&g_ctrl.req, h.kind, h.merged) && !(c == 0 && attach_refused());
}
int ctrl_dialog_ready_count(int* first) {
    int c, n = 0;
    *first = -1;
    for (c = 0; c < cards(); ++c)
        if (ctrl_dialog_ready(c)) {
            if (*first < 0) *first = c;
            ++n;
        }
    return n;
}
/* the players still missing for the least, as "player 2" or "players 2 and 3": how many */
static int missing(char* out, size_t size) {
    int c, n = 0, list[CTRL_PLAYERS];
    size_t at = 0;
    for (c = 0; c < g_ctrl.req.min && c < cards(); ++c)
        if (!ctrl_dialog_ready(c)) list[n++] = c;
    if (size) out[0] = 0;
    for (c = 0; c < n && at < size; ++c) {
        int w = snprintf(out + at, size - at, "%s%d", c == 0 ? (n == 1 ? "player " : "players ")
                                                         : c + 1 == n ? " and "
                                                                      : ", ",
                         list[c] + 1);
        at += w > 0 ? (size_t)w : 0;
    }
    return n;
}
/* the first card that nothing plays, other than but, else player 1 when it has the keyboard alone (which still plays
   it with the controller): -1 for none */
static int free_card(int but) {
    int c;
    for (c = 0; c < cards(); ++c)
        if (c != but && !holder(c).held) return c;
    return but != 0 && !g_ctrl.pad[0].present ? 0 : -1;
}
static CtrlDo did(int what, int from, int to) { return (CtrlDo){what, from, to, -1, 0}; }
const char* ctrl_found_name(const CtrlFound* f) {
    return f->pro ? "Pro Controller 2" : f->right ? "Joy-Con 2 (R)" : "Joy-Con 2 (L)";
}
/* whether player c's controller is one the runtime connected itself, which the dialog can disconnect */
static int droppable(int c) { return c < cards() && g_ctrl.pad[c].present && g_ctrl.pad[c].joycon2; }
/* the game may play with the Joy-Con 2 as a mouse: it takes only Joy-Con 2 styles and started the mouse sensor (which
   one game does at setup for every scene, so this never says it does) */
static int maybe_mouse(void) { return g_ctrl.req.mode == CTRL_MODE_SUPPORT && g_ctrl.req.mouse && g_ctrl.req.mouse_hands; }
/* the items shown, in order: Continue (Close for the modes that only explain), Cancel, Detach, the PC mouse in the
   mouse case, Disconnect for each player whose controller the runtime connected itself, and Connect for each new
   controller waiting to sync (a click confirms it) or one held off since a Disconnect. How many */
static int item_list(int out[ITEMS]) {
    int n = 0, i;
    out[n++] = ITEM_CONTINUE;
    if (g_ctrl.req.mode != CTRL_MODE_SUPPORT || g_ctrl.req.lost[0]) return n;
    out[n++] = ITEM_CANCEL;
    if (g_ctrl.can_attach) out[n++] = ITEM_DETACH;
    if (maybe_mouse()) out[n++] = ITEM_PC_MOUSE;
    for (i = 0; i < CTRL_PLAYERS; ++i)
        if (droppable(i)) out[n++] = ITEM_DROP + i;
    for (i = 0; i < g_ctrl.nfound; ++i)
        if ((!g_ctrl.found[i].known && g_ctrl.found[i].syncing && !g_ctrl.found[i].asked) || g_ctrl.found[i].held)
            out[n++] = ITEM_TAKE + i;
    return n;
}
static CtrlDo act(int item) {
    char who[64];
    int first;
    changed();
    if (item == ITEM_CANCEL || g_ctrl.req.mode != CTRL_MODE_SUPPORT || g_ctrl.req.lost[0]) {
        g_ctrl.answer = item == ITEM_CANCEL && g_ctrl.req.mode == CTRL_MODE_SUPPORT ? CANCELLED : CONTINUED;
        atomic_store_explicit(&g_ctrl.shown, 0, memory_order_release);
        return did(CTRL_DO_ANSWERED, 0, 0);
    }
    if (item == ITEM_CONTINUE) {
        if (missing(who, sizeof who) || (g_ctrl.req.min > 0 && !ctrl_dialog_ready_count(&first))) {
            g_ctrl.refused = 1;
            return did(CTRL_DO_NONE, 0, 0);
        }
        g_ctrl.answer = CONTINUED;
        atomic_store_explicit(&g_ctrl.shown, 0, memory_order_release);
        return did(CTRL_DO_ANSWERED, 0, 0);
    }
    if (item == ITEM_DETACH) return did(CTRL_DO_DETACH, g_ctrl.attached, 0);
    if (item == ITEM_PC_MOUSE) {
        g_ctrl.pc_mouse = !g_ctrl.pc_mouse;
        return did(CTRL_DO_PC_MOUSE, g_ctrl.pc_mouse, 0);
    }
    if (item >= ITEM_DROP && item < ITEM_DROP + CTRL_PLAYERS) {
        snprintf(g_ctrl.said, sizeof g_ctrl.said, "%.47s disconnected, player %d waits.",
                 g_ctrl.pad[item - ITEM_DROP].name, item - ITEM_DROP + 1);
        if (g_ctrl.focus == item) g_ctrl.focus = ITEM_CONTINUE; /* its item goes with the controller */
        return did(CTRL_DO_DISCONNECT, item - ITEM_DROP, 0);
    }
    if (item >= ITEM_TAKE && item < ITEM_TAKE + g_ctrl.nfound) {
        g_ctrl.found[item - ITEM_TAKE].asked = 1;
        return did(CTRL_DO_TAKE, item - ITEM_TAKE, 0);
    }
    return did(CTRL_DO_NONE, 0, 0);
}
static void focus_move(int step) {
    int list[ITEMS], n = item_list(list), i;
    for (i = 0; i < n && list[i] != g_ctrl.focus; ++i) {}
    g_ctrl.focus = list[((i + step) % n + n) % n];
    changed();
}
#define B_A       (1ull << 0)
#define B_B       (1ull << 1)
#define B_X       (1ull << 2)
#define B_L       (1ull << 6)
#define B_R       (1ull << 7)
#define B_UP      (1ull << 13 | 1ull << 17)
#define B_DOWN    (1ull << 15 | 1ull << 19)
#define B_LEFT_S  (1ull << 24 | 1ull << 25) /* a left Joy-Con's SL and SR */
#define B_RIGHT_S (1ull << 26 | 1ull << 27)
/* a gesture refused: why, in the card it would have filled (or the controller's own), and in the line under them */
static CtrlDo refuse(int player, int card, const char* why) {
    g_ctrl.refused_card = card;
    snprintf(g_ctrl.refusal, sizeof g_ctrl.refusal, "%s", why);
    snprintf(g_ctrl.problem, sizeof g_ctrl.problem, "%.47s: %s", g_ctrl.pad[player].name, why);
    changed();
    return did(CTRL_DO_NONE, 0, 0);
}
/* a gesture taken: the card the controller of player goes to (-1 when it has one already, or none is free) */
static CtrlDo gesture(int what, int player, int to, int side, const char* style) {
    CtrlDo d = did(what, player, to);
    int card = player < cards() ? -1 : free_card(player);
    changed();
    g_ctrl.refused_card = -1;
    g_ctrl.problem[0] = 0;
    if (player >= cards() && card < 0) {
        snprintf(g_ctrl.problem, sizeof g_ctrl.problem, "Every player the game takes has a controller already.");
        return did(CTRL_DO_NONE, 0, 0);
    }
    d.card = card;
    d.side = side;
    snprintf(g_ctrl.said, sizeof g_ctrl.said, "%.47s is player %d now, %s.", g_ctrl.pad[player].name,
             (card >= 0 ? card : player) + 1, style);
    return d;
}
CtrlDo ctrl_dialog_press(int player, u64 pressed, u64 held) {
    u64 now = pressed | held;
    const CtrlPad* p;
    int q, target;
    if (!ctrl_dialog_shown() || player < 0 || player >= CTRL_PLAYERS) return did(CTRL_DO_NONE, 0, 0);
    g_ctrl.held[player] = held;
    if (!pressed) return did(CTRL_DO_NONE, 0, 0);
    if (g_ctrl.req.mode != CTRL_MODE_SUPPORT) return pressed & (B_A | B_B) ? act(ITEM_CONTINUE) : did(CTRL_DO_NONE, 0, 0);
    if (pressed & B_A) return act(g_ctrl.focus); /* A only works the items */
    if ((pressed & B_B) && !g_ctrl.req.lost[0]) return act(ITEM_CANCEL); /* a pause's dialog has no Cancel */
    if ((pressed & B_X) && player < cards()) return did(CTRL_DO_IDENTIFY, player, 0);
    if (pressed & B_UP) focus_move(-1);
    if (pressed & B_DOWN) focus_move(1);
    p = &g_ctrl.pad[player];
    if (!p->present) return did(CTRL_DO_NONE, 0, 0);
    target = player < cards() ? player : free_card(player);
    /* SL and SR together on one Joy-Con: that Joy-Con alone, held sideways */
    for (q = 0; q < 2 && p->kind != CTRL_PAD_HOST; ++q) {
        u64 s = q ? B_RIGHT_S : B_LEFT_S;
        if ((now & s) != s || !(pressed & s)) continue;
        if (p->kind == CTRL_PAD_JOY_PAIR && p->split) return did(CTRL_DO_NONE, 0, 0); /* split already */
        if (p->kind == CTRL_PAD_JOY_LEFT + q || (p->kind == CTRL_PAD_JOY_PAIR)) {
            if (!single_ok(q))
                return refuse(player, target,
                              pair_ok(0) ? "this game does not take one Joy-Con sideways: press L and R on two "
                                           "Joy-Cons together"
                                         : "this game does not take one Joy-Con sideways: L+R on a Pro Controller");
            return gesture(p->kind == CTRL_PAD_JOY_PAIR ? CTRL_DO_SPLIT : CTRL_DO_SINGLE, player, 0, q,
                           "one Joy-Con held sideways");
        }
    }
    /* L and R together on one controller: a pair or a pad as a whole one */
    if ((now & (B_L | B_R)) == (B_L | B_R) && (pressed & (B_L | B_R)) &&
        (p->kind == CTRL_PAD_HOST || p->kind == CTRL_PAD_JOY_PAIR || p->kind == CTRL_PAD_PRO)) {
        if (p->kind == CTRL_PAD_JOY_PAIR && !(g_ctrl.req.styles & CTRL_STYLE_DUAL))
            return refuse(player, target,
                          "this game does not take both Joy-Cons as one player: press SL and SR on one of them");
        if (p->kind == CTRL_PAD_PRO && misfit_kind(&g_ctrl.req, CTRL_PAD_PRO, 0))
            return refuse(player, target, misfit_kind(&g_ctrl.req, CTRL_PAD_PRO, 0));
        return gesture(CTRL_DO_WHOLE, player, 0, 0,
                       p->kind == CTRL_PAD_HOST  ? ctrl_style_words(&g_ctrl.req)
                       : p->kind == CTRL_PAD_PRO ? "a Pro Controller"
                                                 : "two Joy-Cons together");
    }
    /* L on a left Joy-Con and R on a right one, held together: one pair on the left one's player */
    if ((p->kind == CTRL_PAD_JOY_LEFT && (pressed & B_L)) || (p->kind == CTRL_PAD_JOY_RIGHT && (pressed & B_R))) {
        int right = p->kind == CTRL_PAD_JOY_RIGHT;
        for (q = 0; q < CTRL_PLAYERS; ++q) {
            const CtrlPad* o = &g_ctrl.pad[q];
            if (q == player || !o->present || o->kind != (right ? CTRL_PAD_JOY_LEFT : CTRL_PAD_JOY_RIGHT) ||
                !(g_ctrl.held[q] & (right ? B_L : B_R)))
                continue;
            {
                int left = right ? q : player, other = right ? player : q, low = left < other ? left : other;
                int card = low < cards() ? low : free_card(left);
                CtrlDo d = did(CTRL_DO_MERGE, left, other);
                if (!pair_ok(1))
                    return refuse(left, card,
                                  "this game does not take two Joy-Cons as one player: press SL and SR on one of "
                                  "them");
                changed();
                g_ctrl.refused_card = -1;
                g_ctrl.problem[0] = 0;
                if (card < 0) {
                    snprintf(g_ctrl.problem, sizeof g_ctrl.problem,
                             "Every player the game takes has a controller already.");
                    return did(CTRL_DO_NONE, 0, 0);
                }
                d.card = low < cards() ? -1 : card; /* the left one moved first when neither has a card */
                snprintf(g_ctrl.said, sizeof g_ctrl.said, "%.40s and %.40s are player %d now, two Joy-Cons together.",
                         g_ctrl.pad[left].name, g_ctrl.pad[other].name, card + 1);
                return d;
            }
        }
    }
    return did(CTRL_DO_NONE, 0, 0);
}
CtrlDo ctrl_dialog_key(int key) {
    if (!ctrl_dialog_shown()) return did(CTRL_DO_NONE, 0, 0);
    switch (key) {
    case CTRL_KEY_UP: focus_move(-1); break;
    case CTRL_KEY_DOWN: focus_move(1); break;
    case CTRL_KEY_ENTER: return act(g_ctrl.focus);
    case CTRL_KEY_ESC: return g_ctrl.req.lost[0] ? did(CTRL_DO_NONE, 0, 0) : act(ITEM_CANCEL);
    default: break;
    }
    return did(CTRL_DO_NONE, 0, 0);
}
int ctrl_dialog_take(u32* ticket) {
    int answer = g_ctrl.answer;
    if (answer == WAITING) return -1;
    g_ctrl.answer = WAITING;
    *ticket = g_ctrl.ticket;
    return answer == CANCELLED;
}

/* ============================================================ drawing */
typedef struct {
    float x, y, w, h;
} Rect;
typedef struct {
    Rect box, card[CTRL_PLAYERS], item[ITEMS];
    int item_of[ITEMS], items, scale;
} Laid;
static int hit(const Rect* r, float x, float y) { return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
/* text (UTF-8) wrapped into at most max rows of cols letters from x, y: the rows it took */
static int rows_of(SnxUiList* l, float x, float y, int s, int cols, int max, const char* text, u32 rgba) {
    char rows[4][FATAL_ROW], line[FATAL_ROW];
    int n, i;
    if (!text || !text[0] || cols < 1) return 0;
    n = fatal_wrap(text, cols, rows, max < 4 ? max : 4);
    if (n > max) n = max;
    if (n > 4) n = 4;
    for (i = 0; i < n && l; ++i) {
        fatal_cp437(rows[i], line, sizeof line);
        ui_text(l, x, y + 16.0f * (float)s * (float)i, s, line, rgba);
    }
    return n;
}
/* a frame of width t around x, y, w, h */
static void frame(SnxUiList* l, float x, float y, float w, float h, float t, u32 rgba) {
    ui_fill(l, x, y, w, t, rgba);
    ui_fill(l, x, y + h - t, w, t, rgba);
    ui_fill(l, x, y, t, h, rgba);
    ui_fill(l, x + w - t, y, t, h, rgba);
}
static u32 game_color(int c) {
    const u8* k = g_ctrl.req.color[c];
    return 0xFF000000u | (u32)k[2] << 16 | (u32)k[1] << 8 | k[0];
}
/* how a controller joins as the game's styles allow it, in a card's words */
static const char* join_words(void) {
    int single = single_ok(0) || single_ok(1), pair = pair_ok(1);
    if (single && pair) return "SL+SR on one Joy-Con held sideways, L and R on two Joy-Cons together, or L+R on a pad.";
    if (single) return "SL+SR on one Joy-Con held sideways, or L+R on a pad.";
    if (pair) return "L on one Joy-Con and R on the other together, or L+R on a pad.";
    return "L+R on a Pro Controller or a pad.";
}
/* what card c says under its title: its state (who plays it, or why that one doesn't fit) and how to fill it */
static void card_texts(int c, char* state, size_t state_size, char* hint, size_t hint_size) {
    const CtrlRequest* r = &g_ctrl.req;
    const CtrlPad* p = &g_ctrl.pad[c];
    Holder h = holder(c);
    int ready = ctrl_dialog_ready(c), f, i;
    const char* why = h.held ? misfit_kind(r, h.kind, h.merged) : NULL;
    if (c == 0 && h.held && attach_refused())
        snprintf(state, state_size, "x %s, attached to the console", h.name);
    else if (ready)
        snprintf(state, state_size, "\xE2\x88\x9A Ready: %s", h.name);
    else if (p->lost)
        snprintf(state, state_size, "x Its controller left");
    else if (why)
        snprintf(state, state_size, "x %s does not fit", h.name);
    else
        snprintf(state, state_size, "Waiting for a controller");
    hint[0] = 0;
    if (c == 0 && h.held && attach_refused())
        snprintf(hint, hint_size, "%s", attach_refused());
    else if (g_ctrl.refused_card == c && g_ctrl.refusal[0])
        snprintf(hint, hint_size, "T%s.", g_ctrl.refusal + 1);
    else if (ready && h.kind == CTRL_PAD_HOST)
        snprintf(hint, hint_size, "The game sees it as %s.", ctrl_style_words(r));
    else if (why && h.kind == CTRL_PAD_PRO)
        snprintf(hint, hint_size, "T%s.", why + 1);
    else if (why && h.kind == CTRL_PAD_JOY_PAIR)
        snprintf(hint, hint_size, "T%s. SL+SR on one of them: that Joy-Con alone, sideways.", why + 1);
    else if (why)
        snprintf(hint, hint_size, "T%s. %s", why + 1, join_words());
    else if (!ready) {
        for (f = 0, i = -1; f < g_ctrl.nfound && i < 0; ++f)
            if (!g_ctrl.found[f].asked) i = f;
        if (i >= 0 && free_card(-1) == c) {
            const CtrlFound* k = &g_ctrl.found[i];
            snprintf(hint, hint_size, "%s found: %s", ctrl_found_name(k),
                     k->held       ? "disconnected, pick Connect below to take it back."
                     : k->known    ? "it connects on its own, press a button on it."
                     : k->syncing ? "pick Connect below to take it."
                                  : "hold its sync button.");
        } else
            snprintf(hint, hint_size, "%s", join_words());
    }
    if (c == 0 && g_ctrl.can_attach && h.held && !attach_refused()) { /* which player 1 is */
        char rest[256];
        snprintf(rest, sizeof rest, "%s", hint);
        snprintf(hint, hint_size, "%s%s%s", g_ctrl.attached ? "Attached to the console." : "Detached from the console.",
                 rest[0] ? " " : "", rest);
    }
}
/* the rows card c's state (2 at most) and hint (3 at most) take on a card cols letters wide */
static int card_state_rows(int c, int cols) {
    char state[128], hint[256], rows[4][FATAL_ROW];
    card_texts(c, state, sizeof state, hint, sizeof hint);
    return snx_clamp_int(fatal_wrap(state, cols, rows, 2), 1, 2);
}
static int card_hint_rows(int c, int cols) {
    char state[128], hint[256], rows[4][FATAL_ROW];
    card_texts(c, state, sizeof state, hint, sizeof hint);
    return hint[0] ? snx_clamp_int(fatal_wrap(hint, cols, rows, 3), 1, 3) : 0;
}
/* the card of player c: its band in the game's color, its title (the game's text, else "Player N"), required or
   optional, what fills it or why it doesn't fit, and how to fill it */
static void card_draw(SnxUiList* l, const Rect* at, int s, int c) {
    const CtrlRequest* r = &g_ctrl.req;
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, x = at->x + cw, y = at->y + ch / 4;
    int cols = (int)(at->w / cw) - 2, ready = ctrl_dialog_ready(c);
    Holder h = holder(c);
    const char* why = h.held ? misfit_kind(r, h.kind, h.merged) : NULL;
    char title[CTRL_TEXT + 16], state[128], hint[256];
    if (!why && c == 0 && h.held) why = attach_refused();
    u32 band = r->has_colors ? game_color(c) : UI_SOFT;
    if (!l) return;
    frame(l, at->x, at->y, at->w, at->h, (float)s * (ready ? 2.0f : 1.0f), why ? UI_FAILURE : ready ? UI_INK : UI_SOFT);
    ui_fill(l, at->x + (float)s, at->y + (float)s, cw / 2, at->h - 2.0f * (float)s, band);
    if (r->has_texts && r->text[c][0])
        snprintf(title, sizeof title, "P%d  %s", c + 1, r->text[c]);
    else
        snprintf(title, sizeof title, "P%d  Player %d", c + 1, c + 1);
    rows_of(l, x, y, s, cols - 10, 1, title, UI_INK);
    ui_text(l, at->x + at->w - cw * 9.5f, y, s, c < r->min ? "required" : "optional", UI_SOFT);
    if (r->has_colors) ui_fill(l, at->x + at->w - cw * 11.5f, y + ch / 4, cw * 1.5f, ch / 2, band);
    card_texts(c, state, sizeof state, hint, sizeof hint);
    {
        int n = rows_of(l, x, y + ch, s, cols, 2, state, why || g_ctrl.pad[c].lost ? UI_FAILURE : ready ? UI_INK : UI_SOFT);
        rows_of(l, x, y + ch * (float)(1 + n), s, cols, 3, hint, why || g_ctrl.refused_card == c ? UI_FAILURE : UI_SOFT);
    }
}
/* the Joy-Con 2 on its side, the rail side with the sensor down on the table, w wide from x, y: three rows */
static void mouse_picture(SnxUiList* l, float x, float y, float w, int s) {
    float ch = 16.0f * (float)s, body = w * 0.7f, bx = x + (w - body) / 2;
    if (!l) return;
    ui_shape(l, bx, y + ch * 0.25f, body, ch * 1.4f, UI_SHAPE_ROUND, "", UI_SHAPE_FILLED, s, UI_SOFT);
    ui_fill(l, bx + body * 0.08f, y + ch * 1.45f, body * 0.84f, ch * 0.2f, UI_FAILURE); /* the rail and its sensor */
    ui_fill(l, x, y + ch * 1.65f, w, (float)s * 2.0f, UI_INK);                           /* the table */
    ui_text(l, bx + body * 0.08f, y + ch * 1.9f, s, "sensor down", UI_FAILURE);
    ui_text(l, x + w - 8.0f * (float)s * 6, y + ch * 1.9f, s, "table", UI_INK);
}
/* how the game's controller is held, in a box w wide from x, y, three rows high: a Pro Controller, two Joy-Cons
   upright side by side, one Joy-Con held sideways with SL and SR on its rail at the top, or one upright */
static void hold_picture(SnxUiList* l, float x, float y, float w, int s) {
    const CtrlRequest* r = &g_ctrl.req;
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, mid = x + w / 2;
    if (!l) return;
    if (r->styles & CTRL_STYLE_FULL_KEY) {
        ui_shape(l, mid - cw * 5, y + ch * 0.4f, cw * 10, ch * 2.2f, UI_SHAPE_ROUND, "", UI_SHAPE_FILLED, s, UI_SOFT);
        ui_shape(l, mid - cw * 3.5f, y + ch * 0.8f, cw * 2, cw * 2, UI_SHAPE_CIRCLE, "", UI_SHAPE_FILLED, s, UI_INK);
        ui_shape(l, mid + cw * 1.5f, y + ch * 1.4f, cw * 2, cw * 2, UI_SHAPE_CIRCLE, "", UI_SHAPE_FILLED, s, UI_INK);
    } else if (r->styles & CTRL_STYLE_DUAL) {
        ui_shape(l, mid - cw * 3.2f, y + ch * 0.2f, cw * 3, ch * 2.6f, UI_SHAPE_ROUND, "L", 0, s, UI_INK);
        ui_shape(l, mid + cw * 0.2f, y + ch * 0.2f, cw * 3, ch * 2.6f, UI_SHAPE_ROUND, "R", 0, s, UI_INK);
    } else if (r->styles & CTRL_STYLE_SINGLE && r->hold) {
        ui_text(l, mid - cw * 4.5f, y, s, "SL", UI_FAILURE);
        ui_text(l, mid + cw * 2.5f, y, s, "SR", UI_FAILURE);
        ui_fill(l, mid - cw * 5, y + ch, cw * 10, (float)s * 2, UI_FAILURE); /* the rail, toward the player */
        ui_shape(l, mid - cw * 5, y + ch * 1.15f, cw * 10, ch * 1.6f, UI_SHAPE_ROUND, "", UI_SHAPE_FILLED, s, UI_SOFT);
    } else if (r->styles & CTRL_STYLE_SINGLE) {
        ui_shape(l, mid - cw * 1.5f, y + ch * 0.2f, cw * 3, ch * 2.6f, UI_SHAPE_ROUND, "", UI_SHAPE_FILLED, s, UI_SOFT);
    }
}
/* the mouse's live check in a box of w at x, y: on the table or in the air, the cursor's area, R and ZR: four rows */
static void mouse_check(SnxUiList* l, float x, float y, float w, int s) {
    const CtrlMouse* m = &g_ctrl.mouse;
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, ax = x + cw, aw = w - 12 * cw, ay = y + ch, ah = ch * 2.5f;
    const char* where = !m->real[1] ? "No Joy-Con 2 (R) yet"
                        : m->surface[1] ? "On the table"
                                        : "In the air: put it down";
    if (!l) return;
    frame(l, x, y, w, ch * 4, (float)s, UI_SOFT);
    ui_text(l, x + cw, y + ch * 0.1f, s, where, m->real[1] && m->surface[1] ? UI_INK : UI_FAILURE);
    if (aw < cw * 4) aw = cw * 4;
    ui_fill(l, ax, ay, aw, ah, UI_PANEL);
    ui_fill(l, ax + g_ctrl.cursor[0] * (aw - cw / 2), ay + g_ctrl.cursor[1] * (ah - cw / 2), cw / 2, cw / 2, UI_INK);
    ui_shape(l, ax + aw + cw, ay, cw * 4, ch * 1.2f, UI_SHAPE_ROUND, "R", m->r ? UI_SHAPE_FILLED : 0, s,
             m->r ? UI_FAILURE : UI_SOFT);
    ui_shape(l, ax + aw + cw * 6, ay, cw * 4, ch * 1.2f, UI_SHAPE_ROUND, "ZR", m->zr ? UI_SHAPE_FILLED : 0, s,
             m->zr ? UI_FAILURE : UI_SOFT);
}
/* the dialog from y at scale s, cols letters wide from x (l NULL: only where it all goes): its height */
static float lay(SnxUiList* l, float x, float y, int cols, int s, Laid* out) {
    const CtrlRequest* r = &g_ctrl.req;
    float cw = 8.0f * (float)s, ch = 16.0f * (float)s, w = cw * (float)cols, y0 = y, tx = x + cw;
    int tc = cols - 2, n = cards(), per = cols >= 64 && n > 1 ? 2 : 1, c, i, first;
    char head[160], each[320], line[400], who[64];
    memset(out, 0, sizeof *out);
    out->scale = s;
    ctrl_request_say(r, head, sizeof head, each, sizeof each);
    if (l) {
        ui_fill(l, x, y, w, ch, UI_INK);
        ui_text(l, x + cw, y, s, "SCARLETNX  /  CONTROLLERS", UI_PAGE);
    }
    y += ch * 2;
    if (r->mode == CTRL_MODE_SUPPORT && tc >= 56) { /* the words, and beside them how to hold it */
        int rows = rows_of(l, tx, y, s, tc - 16, 2, head, UI_INK);
        rows += rows_of(l, tx, y + ch * (float)rows, s, tc - 16, 3, each, UI_SOFT);
        hold_picture(l, tx + cw * (float)(tc - 14), y, cw * 14, s);
        y += ch * (float)(rows > 3 ? rows : 3);
    } else {
        y += ch * (float)rows_of(l, tx, y, s, tc, 2, head, UI_INK);
        y += ch * (float)rows_of(l, tx, y, s, tc, 3, each, UI_SOFT);
    }
    if (r->mode == CTRL_MODE_SUPPORT) {
        float cwid = (w - 2 * cw - cw * (float)(per - 1)) / (float)per, chgt;
        int hints = 2; /* a state row and a hint row at the least */
        for (c = 0; c < n; ++c)
            hints = snx_clamp_int(card_state_rows(c, (int)(cwid / cw) - 2) + card_hint_rows(c, (int)(cwid / cw) - 2),
                                  hints, 5);
        chgt = ch * ((float)hints + 1.5f);
        y += ch / 2;
        for (c = 0; c < n; ++c) {
            Rect* at = &out->card[c];
            at->x = tx + (cwid + cw) * (float)(c % per);
            at->y = y + (chgt + ch / 2) * (float)(c / per);
            at->w = cwid;
            at->h = chgt;
            card_draw(l, at, s, c);
        }
        y += (chgt + ch / 2) * (float)((n + per - 1) / per);
        if (missing(who, sizeof who))
            snprintf(line, sizeof line, "Missing: %s (%s%s).", who, ctrl_style_words(r),
                     strstr(who, " and ") ? " each" : "");
        else if (ctrl_dialog_ready_count(&first) < n)
            snprintf(line, sizeof line, "Ready. More players can still join.");
        else
            snprintf(line, sizeof line, "Ready.");
        y += ch * (float)rows_of(l, tx, y, s, tc, 2, line, g_ctrl.refused || strncmp(line, "Ready", 5) ? UI_FAILURE : UI_INK);
        for (c = n, line[0] = 0, i = 0; c < CTRL_PLAYERS; ++c) /* the controllers the game doesn't see yet */
            if (g_ctrl.pad[c].present && !merged_into(c)) {
                size_t at = strlen(line);
                snprintf(line + at, sizeof line - at, "%s%s", i++ ? ", " : "Also here: ", g_ctrl.pad[c].name);
            }
        if (i) {
            size_t at = strlen(line);
            snprintf(line + at, sizeof line - at, ". L+R, or SL+SR on a Joy-Con, makes %s a player.", i > 1 ? "one" : "it");
            y += ch * (float)rows_of(l, tx, y, s, tc, 2, line, UI_SOFT);
        }
        if (g_ctrl.said[0]) y += ch * (float)rows_of(l, tx, y, s, tc, 1, g_ctrl.said, UI_INK);
    }
    if (maybe_mouse()) { /* optional: the game may use the Joy-Con 2 as a mouse, it never says so */
        float half = (w - 3 * cw) / 2;
        y += ch / 2;
        y += ch * (float)rows_of(l, tx, y, s, tc, 2,
                                 "If the game uses the Joy-Con 2 as a mouse: on its side on the table, the sensor down. "
                                 "R and ZR click.",
                                 UI_SOFT);
        mouse_picture(l, tx, y, half, s);
        mouse_check(l, tx + half + cw, y, half, s);
        y += ch * 4.5f;
        if (g_ctrl.mouse.rate > 0 && g_ctrl.mouse.rate < 40) {
            snprintf(line, sizeof line,
                     "It reports only %d times a second, so a cursor would jump: see the connection interval tip in "
                     "Controllers > Bluetooth.",
                     g_ctrl.mouse.rate);
            y += ch * (float)rows_of(l, tx, y, s, tc, 2, line, UI_WARNING);
        }
    }
    if (g_ctrl.problem[0]) y += ch * (float)rows_of(l, tx, y, s, tc, 2, g_ctrl.problem, UI_WARNING);
    y += ch;
    /* the items, one in focus */
    {
        int list[ITEMS], k = item_list(list);
        for (i = 0; i < k; ++i) {
            int item = list[i], on = item == g_ctrl.focus,
                off = item == ITEM_CONTINUE && r->mode == CTRL_MODE_SUPPORT && !r->lost[0] && missing(who, sizeof who);
            u32 fg = on ? UI_PAGE : off ? UI_SOFT : UI_INK;
            if (item == ITEM_CONTINUE && r->lost[0])
                snprintf(line, sizeof line, "Go on without %s", r->lost);
            else if (item == ITEM_CONTINUE)
                snprintf(line, sizeof line, "%s",
                         r->mode != CTRL_MODE_SUPPORT ? "Close" : off ? "Continue (players missing)" : "Continue");
            else if (item == ITEM_CANCEL)
                snprintf(line, sizeof line, "Cancel");
            else if (item == ITEM_DETACH)
                snprintf(line, sizeof line, "%s", g_ctrl.attached ? "Detach the Joy-Cons from the console (tabletop)"
                                                                  : "Attach the Joy-Cons to the console");
            else if (item == ITEM_PC_MOUSE)
                snprintf(line, sizeof line, "If it uses a mouse, the PC mouse as the right one: %s",
                         g_ctrl.pc_mouse ? "On" : "Off");
            else if (item >= ITEM_DROP && item < ITEM_TAKE)
                snprintf(line, sizeof line, "Disconnect P%d: %s", item - ITEM_DROP + 1,
                         g_ctrl.pad[item - ITEM_DROP].name);
            else if (g_ctrl.found[item - ITEM_TAKE].held)
                snprintf(line, sizeof line, "Connect %s again", ctrl_found_name(&g_ctrl.found[item - ITEM_TAKE]));
            else
                snprintf(line, sizeof line, "Connect the new %s", ctrl_found_name(&g_ctrl.found[item - ITEM_TAKE]));
            out->item[out->items] = (Rect){x, y, w, ch};
            out->item_of[out->items++] = item;
            if (l) {
                char drawn[sizeof line + 4];
                if (on) ui_fill(l, x, y, w, ch, off ? UI_SOFT : UI_INK);
                snprintf(drawn, sizeof drawn, "\xFE %s", line);
                ui_text(l, x + cw, y, s, drawn, fg);
            }
            y += ch;
        }
    }
    y += ch;
    {
        /* the gestures the game's styles take: SL+SR only where it takes a single Joy-Con, which alone is held
           sideways */
        int single = single_ok(0) || single_ok(1);
        char keys[200];
        snprintf(keys, sizeof keys, "Enter or A: pick.%s X: identify. Join: %s.", r->lost[0] ? "" : " Esc or B: Cancel.",
                 single && pair_ok(1) ? "L+R, or SL+SR on a Joy-Con sideways"
                 : single             ? "SL+SR on a Joy-Con sideways, or L+R on a pad"
                 : pair_ok(1)         ? "L on one Joy-Con and R on the other, or L+R on a pad"
                                      : "L+R on a pad");
        y += ch * (float)rows_of(l, tx, y, s, tc, 2, keys, UI_SOFT);
    }
    out->box = (Rect){x, y0, w, y - y0};
    return y - y0;
}
/* where it all goes on sw x sh, the letters at scale at most: the scale that fits, the box placed in the middle */
static void place(SnxUiList* l, int sw, int sh, int scale, Laid* out) {
    int s, cols;
    float h = 0;
    for (s = scale < 1 ? 1 : scale; s >= 1; --s) {
        cols = snx_clamp_int(sw * 7 / 8 / (8 * s), 40, 96);
        if (cols * 8 * s > sw) cols = sw / (8 * s);
        h = lay(NULL, 0, 0, cols, s, out);
        if (h <= (float)sh || s == 1) break;
    }
    {
        float x = (float)((sw - cols * 8 * s) / 2), y = h < (float)sh ? ((float)sh - h) / 2 : 0;
        if (l) ui_fill(l, x, y, (float)(cols * 8 * s), h, UI_PAGE);
        lay(l, x, y, cols, s, out);
    }
}
void ctrl_dialog_draw(SnxUiList* l, int sw, int sh, int scale) {
    Laid at;
    if (!ctrl_dialog_shown()) return;
    place(l, sw, sh, scale, &at);
}
CtrlDo ctrl_dialog_tap(float x, float y, int sw, int sh, int scale) {
    Laid at;
    int i;
    if (!ctrl_dialog_shown()) return did(CTRL_DO_NONE, 0, 0);
    place(NULL, sw, sh, scale, &at);
    for (i = 0; i < at.items; ++i)
        if (hit(&at.item[i], x, y)) {
            g_ctrl.focus = at.item_of[i];
            return act(at.item_of[i]);
        }
    for (i = 0; i < cards(); ++i)
        if (hit(&at.card[i], x, y) && g_ctrl.pad[i].present) return did(CTRL_DO_IDENTIFY, i, 0);
    return did(CTRL_DO_NONE, 0, 0);
}
