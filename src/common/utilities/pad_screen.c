/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/pad_screen.h"
#include <stdio.h>
#include <string.h>

/* the controls' boxes, as the CONTROLS page has them: the left side's on the left, top to bottom, then the right
   side's */
static const int ORDER[SNX_PAD_CONTROLS] = {
    SNX_PAD_LT, SNX_PAD_LB, SNX_PAD_UP,    SNX_PAD_DOWN, SNX_PAD_LEFT, SNX_PAD_RIGHT, SNX_PAD_LS, SNX_PAD_BACK,
    SNX_PAD_RT, SNX_PAD_RB, SNX_PAD_NORTH, SNX_PAD_WEST, SNX_PAD_EAST, SNX_PAD_SOUTH, SNX_PAD_RS, SNX_PAD_START};
#define SIDE (SNX_PAD_CONTROLS / 2)

void snx_pad_screen_start(SnxPadScreen* s) {
    *s = (SnxPadScreen){0};
    snx_pad_map_unset(&s->own);
    s->focus = ORDER[0];
}
/* the pad's labels, NULL for none known: an Xbox pad's */
static const int* labels_of(const SnxPadScreen* s) {
    for (int c = 0; c < 4; ++c)
        if (s->labels[c] != SNX_PAD_LABEL_NONE) return s->labels;
    return NULL;
}
void snx_pad_screen_map(const SnxPadScreen* s, SnxPadMap* map) {
    SnxPadMap placed;
    snx_pad_map_position(&placed);
    *map = s->own;
    snx_pad_map_fill(map, &placed);
    if (s->swap_ab) snx_pad_map_swap_ab(map);
}
/* map, as shown, the player's from now on: kept without the swap, which goes over it */
static int own(SnxPadScreen* s, const SnxPadMap* map) {
    s->own = *map;
    if (s->swap_ab) snx_pad_map_swap_ab(&s->own);
    return SNX_PAD_DID_MAP;
}
static int at_of(int control) {
    int i;
    for (i = 0; i < SNX_PAD_CONTROLS && ORDER[i] != control; ++i) {}
    return i;
}
int snx_pad_screen_key(SnxPadScreen* s, int key) {
    int control = s->focus;
    if (s->listening) {
        if (key == SNX_PAD_KEY_BACK) s->listening = 0;
        return SNX_PAD_DID_NOTHING;
    }
    if (key == SNX_PAD_KEY_UP || key == SNX_PAD_KEY_DOWN) {
        int at = (at_of(control) + (key == SNX_PAD_KEY_UP ? SNX_PAD_CONTROLS - 1 : 1)) % SNX_PAD_CONTROLS;
        s->focus = ORDER[at];
    } else if (key == SNX_PAD_KEY_LEFT || key == SNX_PAD_KEY_RIGHT) { /* None, then A to Down, round */
        SnxPadMap map;
        snx_pad_screen_map(s, &map);
        map.bit[control] = (signed char)((map.bit[control] + 1 + (key == SNX_PAD_KEY_LEFT ? SNX_PAD_BUTTONS : 1)) %
                                             (SNX_PAD_BUTTONS + 1) -
                                         1);
        return own(s, &map);
    } else if (key == SNX_PAD_KEY_SELECT)
        s->listening = s->pad[0] != 0; /* a press needs a pad */
    else if (key == SNX_PAD_KEY_BACK)
        return SNX_PAD_DID_LEAVE;
    return SNX_PAD_DID_NOTHING;
}
int snx_pad_screen_press(SnxPadScreen* s, int control) {
    int focused = s->focus;
    SnxPadMap map;
    signed char bit;
    if (!s->listening || control < 0 || control >= SNX_PAD_CONTROLS) return SNX_PAD_DID_NOTHING;
    s->listening = 0;
    if (control == focused) return SNX_PAD_DID_NOTHING;
    snx_pad_screen_map(s, &map);
    bit = map.bit[control];
    map.bit[control] = map.bit[focused];
    map.bit[focused] = bit;
    s->focus = control; /* where the button went */
    return own(s, &map);
}
int snx_pad_screen_tap(SnxPadScreen* s, int id) {
    if (id < 0 || id >= SNX_PAD_CONTROLS || s->listening) return SNX_PAD_DID_NOTHING;
    if (s->focus == id) return snx_pad_screen_key(s, SNX_PAD_KEY_SELECT);
    s->focus = id;
    return SNX_PAD_DID_NOTHING;
}

/* ---- the layout ---- */
static SnxPadShape* add(SnxPadDrawing* d, int kind, int style, int id, float x, float y, float w, float h) {
    if (d->n == SNX_PAD_SHAPES) return NULL;
    SnxPadShape* p = &d->shape[d->n++];
    p->kind = (unsigned char)kind;
    p->style = (unsigned char)style;
    p->id = (short)id;
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->text = NULL;
    return p;
}
/* text from x, y, cut to chars glyphs */
static void text(SnxPadDrawing* d, int style, float x, float y, float cw, float ch, const char* s, int chars) {
    int n = (int)strlen(s), room = (int)sizeof d->text - d->used - 1;
    SnxPadShape* p;
    if (n > chars) n = chars;
    if (n > room) n = room;
    if (n <= 0 || !(p = add(d, SNX_PAD_SHAPE_TEXT, style, -1, x, y, (float)n * cw, ch))) return;
    memcpy(d->text + d->used, s, (size_t)n);
    d->text[d->used + n] = 0;
    p->text = d->text + d->used;
    d->used += n + 1;
}
/* a box with its text a glyph in from its left, in the middle of its height */
static void box(SnxPadDrawing* d, int style, int id, float x, float y, float w, float h, float cw, float ch,
                const char* s) {
    if (!add(d, SNX_PAD_SHAPE_BOX, style, id, x, y, w, h)) return;
    text(d, style, x + cw, y + (h - ch) / 2, cw, ch, s, (int)(w / cw) - 2);
}
static int style_of(const SnxPadScreen* s, int control) {
    if (s->focus == control) return SNX_PAD_STYLE_FOCUS;
    return s->held >> control & 1 ? SNX_PAD_STYLE_LIT : SNX_PAD_STYLE_PLAIN;
}
/* a face button's glyph: its letter, or the symbol's in the font */
static const char* glyph(const SnxPadScreen* s, int control) {
    static const char* const SYMBOLS[] = {"x", "o", "\xFE", "\x1E"};
    int label = labels_of(s) ? s->labels[control] : SNX_PAD_LABEL_NONE;
    if (label >= SNX_PAD_LABEL_CROSS && label <= SNX_PAD_LABEL_TRIANGLE) return SYMBOLS[label - SNX_PAD_LABEL_CROSS];
    return snx_pad_control_name(control, labels_of(s));
}
/* a part of the pad, and its name in it when it fits */
static void part(SnxPadDrawing* d, const SnxPadScreen* s, int kind, int control, float x, float y, float w, float h,
                 float cw, float ch, const char* name) {
    int n = name ? (int)strlen(name) : 0;
    add(d, kind, style_of(s, control), control, x, y, w, h);
    if (n && (float)n * cw <= w && ch <= h)
        text(d, style_of(s, control), x + (w - (float)n * cw) / 2, y + (h - ch) / 2, cw, ch, name, n);
}
/* the pad in w x h at x, y, its height 0.62 of its width: the parts' places as fractions of the width, from the
   Xbox pad's and the PlayStation one's, none over another */
static void pad(SnxPadDrawing* d, const SnxPadScreen* s, float x, float y, float w, float cw, float ch) {
    float h = w * 0.62f, stick = 0.09f * w, face = 0.045f * w, apart = 0.095f * w, arm = 0.075f * w, thick = 0.055f * w;
    float lsx = s->symmetric ? 0.38f : 0.24f, lsy = s->symmetric ? 0.80f : 0.45f;
    float dx = (s->symmetric ? 0.24f : 0.38f) * w, dy = (s->symmetric ? 0.45f : 0.80f) * h;
    float fx = 0.76f * w, fy = 0.48f * h; /* the top one clear of RB */
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LT, x + 0.10f * w, y, 0.20f * w, 0.12f * h, cw, ch, "LT");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RT, x + 0.70f * w, y, 0.20f * w, 0.12f * h, cw, ch, "RT");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LB, x + 0.10f * w, y + 0.14f * h, 0.20f * w, 0.08f * h, cw, ch, "LB");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RB, x + 0.70f * w, y + 0.14f * h, 0.20f * w, 0.08f * h, cw, ch, "RB");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_BACK, x + 0.41f * w, y + 0.37f * h, 0.06f * w, 0.06f * h, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_START, x + 0.53f * w, y + 0.37f * h, 0.06f * w, 0.06f * h, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_DISC, SNX_PAD_LS, x + lsx * w - stick, y + lsy * h - stick, 2 * stick, 2 * stick, cw, ch,
         NULL);
    part(d, s, SNX_PAD_SHAPE_DISC, SNX_PAD_RS, x + 0.62f * w - stick, y + 0.80f * h - stick, 2 * stick, 2 * stick, cw,
         ch, NULL);
    /* the D-pad: an arm each way round a middle that is none of them */
    add(d, SNX_PAD_SHAPE_BAR, SNX_PAD_STYLE_PLAIN, -1, x + dx - thick / 2, y + dy - thick / 2, thick, thick);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_UP, x + dx - thick / 2, y + dy - thick / 2 - arm, thick, arm, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_DOWN, x + dx - thick / 2, y + dy + thick / 2, thick, arm, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LEFT, x + dx - thick / 2 - arm, y + dy - thick / 2, arm, thick, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RIGHT, x + dx + thick / 2, y + dy - thick / 2, arm, thick, cw, ch, NULL);
    for (int c = SNX_PAD_SOUTH; c <= SNX_PAD_NORTH; ++c) {
        float ox = c == SNX_PAD_WEST ? -apart : c == SNX_PAD_EAST ? apart : 0;
        float oy = c == SNX_PAD_NORTH ? -apart : c == SNX_PAD_SOUTH ? apart : 0;
        part(d, s, SNX_PAD_SHAPE_DISC, c, x + fx + ox - face, y + fy + oy - face, 2 * face, 2 * face, cw, ch,
             glyph(s, c));
    }
}
/* a control's box: its name on the left, the button it sends on the right */
static void control_box(SnxPadDrawing* d, int style, int id, float x, float y, float w, float h, float cw, float ch,
                        const char* name, const char* button) {
    int room = (int)(w / cw) - 2, n = (int)strlen(name), b = (int)strlen(button);
    char line[32];
    if (n + 1 + b > room) { /* too narrow for both apart */
        snprintf(line, sizeof line, "%s %s", name, button);
        box(d, style, id, x, y, w, h, cw, ch, line);
        return;
    }
    box(d, style, id, x, y, w, h, cw, ch, name);
    text(d, style, x + w - (float)(b + 1) * cw, y + (h - ch) / 2, cw, ch, button, b);
}
/* the length of s's next line of cols glyphs at most: to its last space, else (a path longer than a line) to its last
   slash */
static int line_of(const char* s, int cols) {
    int n = (int)strlen(s), i, space = 0, slash = 0;
    if (n <= cols) return n;
    for (i = 0; i < cols; ++i)
        if (s[i] == ' ')
            space = i + 1;
        else if (s[i] == '/' || s[i] == '\\')
            slash = i + 1;
    return space ? space : slash ? slash : cols;
}
/* with no pad, in the middle: that there is none, and why (the note) or what to do, wrapped */
static void no_pad(SnxPadDrawing* d, const SnxPadScreen* s, float x, float y, float w, float h, float cw, float ch) {
    static const char TITLE[] = "No controller";
    const char *body = s->note[0] ? s->note : "Plug one in: what it presses lights up here", *p;
    int cols = (int)(w / cw), lines = 0;
    if (cols < 8 || h < ch) return;
    for (p = body; *p; ++lines) {
        p += line_of(p, cols);
        while (*p == ' ') ++p;
    }
    float ly = y + (h - (float)(lines + 2) * ch) / 2;
    if (ly < y) ly = y;
    int n = (int)sizeof TITLE - 1 < cols ? (int)sizeof TITLE - 1 : cols;
    text(d, SNX_PAD_STYLE_SOFT, x + (w - (float)n * cw) / 2, ly, cw, ch, TITLE, n);
    for (p = body, ly += 2 * ch; *p && ly + ch <= y + h; ly += ch) {
        int len = line_of(p, cols), shown = len;
        while (shown > 0 && p[shown - 1] == ' ') --shown;
        text(d, s->note[0] ? SNX_PAD_STYLE_NOTE : SNX_PAD_STYLE_SOFT, x + (w - (float)shown * cw) / 2, ly, cw, ch, p,
             shown);
        p += len;
        while (*p == ' ') ++p;
    }
}
/* what the focus does, in the bar */
static void say(const SnxPadScreen* s, char* out, size_t size, const char** keys) {
    SnxPadMap map;
    snx_pad_screen_map(s, &map);
    if (s->listening)
        snprintf(out, size, "Press a button on the pad for %s", snx_pad_button_name(map.bit[s->focus]));
    else
        snprintf(out, size, "%s sends %s", snx_pad_control_name(s->focus, labels_of(s)),
                 snx_pad_button_name(map.bit[s->focus]));
    *keys = s->listening ? "Esc Stop"
            : s->pad[0]  ? "\x1B\x1A Change  Enter Press  Esc Back"
                         : "\x1B\x1A Change  Esc Back";
}
void snx_pad_screen_lay(const SnxPadScreen* s, float x, float y, float w, float h, float cw, float ch,
                        SnxPadDrawing* d) {
    enum { MARGIN = 1, GAP = 2, BOX = 16, PAD_MIN = 18 };
    int wide = w >= cw * (2 * MARGIN + BOX * 2 + GAP * 2 + PAD_MIN), c;
    float left = x + MARGIN * cw, right = x + w - MARGIN * cw, top = y + ch / 2, bottom = y + h - 2.25f * ch;
    float bw = BOX * cw, rows_x[2];
    char bar[160];
    const char* keys;
    SnxPadMap map;
    d->n = d->used = 0;
    snx_pad_screen_map(s, &map);
    /* the bar of what the focus does, across the bottom; the keys at its right when they fit */
    say(s, bar, sizeof bar, &keys);
    if (add(d, SNX_PAD_SHAPE_BOX, SNX_PAD_STYLE_PLAIN, -1, left, y + h - 1.75f * ch, right - left, 1.5f * ch)) {
        int room = (int)((right - left) / cw) - 2, n = (int)strlen(keys);
        float by = y + h - 1.5f * ch;
        if ((int)strlen(bar) + 2 + n > room) n = 0;
        text(d, SNX_PAD_STYLE_PLAIN, left + cw, by, cw, ch, bar, room - (n ? n + 2 : 0));
        if (n) text(d, SNX_PAD_STYLE_SOFT, right - (float)(n + 1) * cw, by, cw, ch, keys, n);
    }
    /* the controls' boxes: two columns of eight, either side of the pad or under it */
    if (!wide) bw = (right - left - GAP * cw) / 2;
    rows_x[0] = left;
    rows_x[1] = right - bw;
    float rows_y = top;
    float step = (bottom - rows_y) / SIDE;
    if (!wide) step = step / 2 > 1.75f * ch ? 1.75f * ch : step / 2; /* the pad takes the rest above them */
    if (step > 2 * ch) step = 2 * ch;
    if (step < ch + 2) step = ch + 2;
    float bh = step >= 1.75f * ch ? 1.5f * ch : step - 2;
    if (!wide) rows_y = bottom - step * SIDE;
    for (int i = 0; i < SNX_PAD_CONTROLS; ++i) {
        float by = rows_y + (float)(i % SIDE) * step;
        c = ORDER[i];
        if (by < top || by + bh > bottom) continue; /* no room: left out */
        control_box(d, style_of(s, c), c, rows_x[i / SIDE], by, bw, bh, cw, ch, snx_pad_control_name(c, labels_of(s)),
                    snx_pad_button_name(map.bit[c]));
    }
    /* between the columns, or over them: the pad named above it, or why there is none */
    float region_x = wide ? rows_x[0] + bw + GAP * cw : left;
    float region_w = wide ? rows_x[1] - GAP * cw - region_x : right - left;
    float region_y = top;
    float region_h = (wide ? bottom : rows_y - ch / 2) - region_y;
    if (!s->pad[0]) {
        no_pad(d, s, region_x, region_y, region_w, region_h, cw, ch);
        return;
    }
    if (region_h >= 2.5f * ch) {
        int n = (int)strlen(s->pad) < (int)(region_w / cw) ? (int)strlen(s->pad) : (int)(region_w / cw);
        text(d, SNX_PAD_STYLE_SOFT, region_x + (region_w - (float)n * cw) / 2, region_y, cw, ch, s->pad, n);
        region_y += 1.5f * ch;
        region_h -= 1.5f * ch;
    }
    float size = region_w < region_h / 0.62f ? region_w : region_h / 0.62f;
    if (size >= PAD_MIN * cw / 2 && region_h >= ch)
        pad(d, s, region_x + (region_w - size) / 2, region_y + (region_h - size * 0.62f) / 2, size, cw, ch);
}
int snx_pad_screen_hit(const SnxPadDrawing* d, float x, float y) {
    for (int i = d->n - 1; i >= 0; --i) {
        const SnxPadShape* p = &d->shape[i];
        if (p->id >= 0 && x >= p->x && y >= p->y && x < p->x + p->w && y < p->y + p->h) return p->id;
    }
    return -1;
}
