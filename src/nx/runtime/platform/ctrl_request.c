/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The argument layouts follow yuzu's applet_controller.h (GPL-2.0-or-later):
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* The controller applet's request (ctrl_applet.h): read from its arguments, said in words, and its result. No drawing,
   so am.c's tests take it alone */
#include "nx/runtime/platform/ctrl_applet.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

/* ============================================================ the request */
/* the argument's sizes: the header, then 4 or 8 colors of 4 bytes, the texts' flag and the texts */
#define ARG_HEADER  7
#define ARG_SIZE(n) (ARG_HEADER + 4 * (n) + 1 + CTRL_TEXT * (n))
#define PRIV_SIZE   0x14

void ctrl_request_base(CtrlRequest* r, u32 styles, int hold) {
    *r = (CtrlRequest){0};
    r->mode = CTRL_MODE_SUPPORT;
    r->slots = 4;
    r->min = r->max = 1;
    r->styles = styles ? styles : CTRL_STYLE_FULL_KEY | CTRL_STYLE_HANDHELD | CTRL_STYLE_JOY_DUAL |
                                      CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT;
    r->hold = hold == 1;
    r->mouse = styles && !(styles & ~(CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2));
}
int ctrl_request_parse(CtrlRequest* r, const u8* common, u64 common_size, const u8* priv, u64 priv_size, const u8* arg,
                       u64 arg_size) {
    int mode = -1, n, i;
    if (common && common_size >= 12) r->version = snx_ld_le32(common + 8);
    /* the private argument: its own size first, then the argument's (which one SDK always gives as the largest
       layout's, whatever it pushes), the mode, the styles and the hold the game had given hid */
    if (priv && priv_size >= PRIV_SIZE && snx_ld_le32(priv) >= PRIV_SIZE) {
        u32 styles = snx_ld_le32(priv + 0xc);
        mode = priv[0xa];
        if (styles) r->styles = styles;
        r->hold = snx_ld_le32(priv + 0x10) == 1;
        r->mouse = !(r->styles & ~(CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2));
    }
    /* the remapping mode has two numbers across SDKs. A mode out of range is told by the argument's size, as some
       games send any byte there */
    if (mode == 3 || mode == 4)
        mode = CTRL_MODE_REMAP;
    else if (mode < 0 || mode > CTRL_MODE_FIRMWARE)
        mode = arg && arg_size == 4 ? CTRL_MODE_FIRMWARE : arg && arg_size == 0x10 ? CTRL_MODE_REMAP : CTRL_MODE_SUPPORT;
    r->mode = mode;
    if (mode != CTRL_MODE_SUPPORT && mode != CTRL_MODE_STRAP_GUIDE) return 0;
    /* the strap guide's own argument is smaller than the support one's: nothing in it is a count */
    if (!arg || arg_size < ARG_HEADER || (mode == CTRL_MODE_STRAP_GUIDE && arg_size < ARG_SIZE(4))) return -1;
    n = arg_size >= ARG_SIZE(8) ? 8 : arg_size >= ARG_SIZE(4) ? 4 : r->version >= 7 ? 8 : 4;
    r->slots = n;
    r->take_over = arg[2] != 0;
    r->left_justify = arg[3] != 0;
    r->permit_dual = arg[4] != 0;
    r->single = arg[5] != 0;
    r->has_colors = arg[6] != 0;
    if (arg_size >= (u64)ARG_HEADER + 4u * (u64)n)
        for (i = 0; i < n; ++i) memcpy(r->color[i], arg + ARG_HEADER + 4 * i, 4);
    else
        r->has_colors = 0;
    if (arg_size >= (u64)ARG_SIZE(n)) {
        r->has_texts = arg[ARG_HEADER + 4 * n] != 0;
        for (i = 0; i < n; ++i) {
            memcpy(r->text[i], arg + ARG_HEADER + 4 * n + 1 + CTRL_TEXT * i, CTRL_TEXT - 1);
            r->text[i][CTRL_TEXT - 1] = 0;
        }
    }
    if (!r->has_texts) memset(r->text, 0, sizeof r->text);
    /* the counts: the least may be 0 (the game takes any), the most at least the least and at most the layout's */
    r->min = snx_clamp_int((signed char)arg[0], 0, n);
    r->max = snx_clamp_int((signed char)arg[1], r->min > 1 ? r->min : 1, n);
    if (r->single) r->min = r->max = 1;
    return 0;
}
int ctrl_request_cards(const CtrlRequest* r) { return r->single ? 1 : snx_clamp_int(r->max, 1, CTRL_PLAYERS); }

/* the words of one style of the game's, NULL for one a player can't take */
static const char* style_word(const CtrlRequest* r, u32 style) {
    switch (style) {
    case CTRL_STYLE_FULL_KEY: return "a Pro Controller";
    case CTRL_STYLE_JOY_DUAL: return "two Joy-Cons together";
    case CTRL_STYLE_JOY_LEFT:
    case CTRL_STYLE_JOY_RIGHT: return r->hold ? "one Joy-Con held sideways" : "one Joy-Con held upright";
    case CTRL_STYLE_JOY_DUAL2: return "two Joy-Con 2 together";
    case CTRL_STYLE_JOY_LEFT2: return r->hold ? "one Joy-Con 2 (L) held sideways" : "one Joy-Con 2 (L) held upright";
    default: return NULL;
    }
}
const char* ctrl_style_words(const CtrlRequest* r) {
    static const u32 order[] = {CTRL_STYLE_FULL_KEY, CTRL_STYLE_JOY_DUAL, CTRL_STYLE_JOY_LEFT, CTRL_STYLE_JOY_RIGHT,
                                CTRL_STYLE_JOY_DUAL2, CTRL_STYLE_JOY_LEFT2};
    size_t i;
    for (i = 0; i < sizeof order / sizeof *order; ++i)
        if (r->styles & order[i]) return style_word(r, order[i]);
    return r->styles & CTRL_STYLE_HANDHELD ? "the handheld controls" : "any controller";
}
int ctrl_host_kind(const CtrlRequest* r) {
    /* hid shows a host pad as a Pro Controller, else as two Joy-Cons, else as two Joy-Con 2: a game that takes
       none of them but single Joy-Cons sees it only once it is one */
    if (r->styles & (CTRL_STYLE_FULL_KEY | CTRL_STYLE_DUAL)) return -1;
    if (r->styles & (CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_LEFT2)) return CTRL_PAD_JOY_LEFT;
    if (r->styles & CTRL_STYLE_JOY_RIGHT) return CTRL_PAD_JOY_RIGHT;
    return -1;
}
void ctrl_request_say(const CtrlRequest* r, char* head, size_t head_size, char* each, size_t each_size) {
    static const u32 order[] = {CTRL_STYLE_FULL_KEY, CTRL_STYLE_JOY_DUAL, CTRL_STYLE_JOY_LEFT, CTRL_STYLE_JOY_DUAL2,
                                CTRL_STYLE_JOY_LEFT2};
    const char* words[5];
    int n = 0, cards = ctrl_request_cards(r), least = r->min > 0 ? r->min : 1;
    size_t i, at;
    for (i = 0; i < sizeof order / sizeof *order; ++i) {
        u32 s = order[i] == CTRL_STYLE_JOY_LEFT ? CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT : order[i];
        if (r->styles & s) words[n++] = style_word(r, order[i]);
    }
    if (r->lost[0]) {
        snprintf(head, head_size, "%s disconnected.", r->lost);
        snprintf(each, each_size, "Reconnect its controller, or press A on another to take its place. The game waits.");
        return;
    }
    if (r->mode == CTRL_MODE_STRAP_GUIDE)
        snprintf(head, head_size, "How to put the wrist straps on the Joy-Cons.");
    else if (r->mode == CTRL_MODE_FIRMWARE)
        snprintf(head, head_size, "The game asks to update the controllers' firmware. That is done on the console.");
    else if (r->mode == CTRL_MODE_REMAP)
        snprintf(head, head_size, "The game asks to change the buttons. That is done in the Controllers page.");
    else if (cards <= least)
        snprintf(head, head_size, "This game needs %d player%s.", cards, cards == 1 ? "" : "s");
    else
        snprintf(head, head_size, "This game needs %d to %d players.", least, cards);
    if (r->mode != CTRL_MODE_SUPPORT) {
        snprintf(each, each_size, "%s", r->mode == CTRL_MODE_STRAP_GUIDE
                                            ? "Slide each strap onto the rail of its Joy-Con, the + and - marks matching."
                                            : "Continue goes back to the game.");
        return;
    }
    at = (size_t)snprintf(each, each_size, cards == 1 ? "1 player: " : "Each player: ");
    if (!n) {
        snprintf(each + at, each_size > at ? each_size - at : 0, "%s.", ctrl_style_words(r));
        return;
    }
    for (i = 0; i < (size_t)n && at < each_size; ++i) {
        int w = snprintf(each + at, each_size - at, "%s%s", i == 0 ? "" : i + 1 == (size_t)n ? " or " : ", ", words[i]);
        at += w > 0 ? (size_t)w : 0;
    }
    if (at < each_size) snprintf(each + at, each_size - at, ".");
}

void ctrl_result_bytes(u8 out[CTRL_RESULT_SIZE], int players, u32 npad_id, int cancelled) {
    memset(out, 0, CTRL_RESULT_SIZE);
    out[0] = (u8)players;
    snx_st_le32(out + 4, npad_id);
    snx_st_le32(out + 8, cancelled ? 2 : 0); /* the console's answer when the player backs out */
}
