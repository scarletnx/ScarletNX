/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/error_panel.h"
#include "common/utilities/fatal_screen.h"
#include "common/utilities/notice.h"
#include "common/utilities/ui_colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_ROWS 160 /* the most rows of a paragraph drawn at once: a 4K picture's at the font's own size */
#define LINE_ROWS    2   /* the error's line and what to do, each: the panel's narrowest holds either in one */
#define SAID_ROWS    3
#define FINGER_ROW   48 /* a finger's row in pixels at the display's scale (src/gui's GUI_FINGER_ROW) */

void snx_error_panel_start(SnxErrorPanel* p, u64 now_ms) {
    int percent = p->percent, fingers = p->fingers, back_key = p->back_key, open_log = p->open_log;
    char pad_do[sizeof p->pad_do], pad_back[sizeof p->pad_back];
    memcpy(pad_do, p->pad_do, sizeof pad_do);
    memcpy(pad_back, p->pad_back, sizeof pad_back);
    *p = (SnxErrorPanel){.shown_ms = now_ms,
                         .pressed = -1,
                         .percent = percent ? percent : 100,
                         .fingers = fingers,
                         .back_key = back_key,
                         .open_log = open_log};
    memcpy(p->pad_do, pad_do[0] ? pad_do : "A", pad_do[0] ? sizeof pad_do : 2);
    memcpy(p->pad_back, pad_back[0] ? pad_back : "B", pad_back[0] ? sizeof pad_back : 2);
    p->input = fingers ? SNX_ERROR_INPUT_TOUCH : SNX_ERROR_INPUT_KEYS;
}

/* the page's items into out: the error's actions, or Details' own */
static int page_items(const SnxErrorPanel* p, const SnxError* e, int out[SNX_ERROR_PANEL_ITEMS]) {
    int n = 0;
    if (!p->details) return snx_error_actions(e, out);
    if (p->open_log && e->log[0]) out[n++] = SNX_ACTION_OPEN_LOG;
    out[n++] = SNX_ACTION_COPY_TEXT;
    out[n++] = SNX_ACTION_BACK;
    return n;
}

/* text at x, y in letters of the font, its UTF-8 as the font's bytes (fatal_cp437), at most cols of them */
static void text_at(SnxUiList* l, float x, float y, int s, const char* text, size_t bytes, int cols, u32 rgba) {
    char row[FATAL_ROW], font[FATAL_ROW];
    if (bytes >= sizeof row) bytes = sizeof row - 1;
    memcpy(row, text, bytes);
    row[bytes] = 0;
    fatal_cp437(row, font, sizeof font);
    if ((int)strlen(font) > cols) font[cols < 0 ? 0 : cols] = 0;
    ui_text(l, x, y, s, font, rgba);
}

/* A paragraph: text wrapped to cols, at most max rows of it from its row first (more: the last shown ends in "..."),
   drawn from y down when l is not NULL. The rows it takes */
typedef struct {
    SnxUiList* l;
    float x;
    int s, cols;
} Pen;
static int para(const Pen* pen, float y, const char* text, int first, int max, u32 rgba, int* total) {
    int at[MESSAGE_ROWS], len[MESSAGE_ROWS];
    int n = fatal_wrap_spans(text, pen->cols, first, at, len, max < MESSAGE_ROWS ? max : MESSAGE_ROWS);
    int shown = n - first < max ? n - first : max;
    if (shown > MESSAGE_ROWS) shown = MESSAGE_ROWS;
    if (total) *total = n;
    for (int i = 0; i < shown && pen->l; ++i) {
        float row_y = y + (float)(i * 16 * pen->s);
        if (i == shown - 1 && first + i < n - 1 && !total) { /* cut: the rest goes */
            char row[FATAL_ROW];
            int keep = len[i] < (int)sizeof row - 4 ? len[i] : (int)sizeof row - 4;
            snprintf(row, sizeof row, "%.*s...", keep, text + at[i]);
            text_at(pen->l, pen->x, row_y, pen->s, row, strlen(row), pen->cols + 3, rgba);
        } else
            text_at(pen->l, pen->x, row_y, pen->s, text + at[i], (size_t)len[i], pen->cols, rgba);
    }
    return shown < 0 ? 0 : shown;
}

/* the foot's line: the keys of the input used last */
static void keys_line(const SnxErrorPanel* p, const SnxError* e, char* out, size_t size) {
    const char* close = p->details ? "back" : e->from_launcher ? "menu" : "close";
    if (p->input == SNX_ERROR_INPUT_PAD)
        snprintf(out, size, "%s: do   D-pad: move   %s: %s", p->pad_do, p->pad_back, close);
    else if (p->input == SNX_ERROR_INPUT_TOUCH)
        snprintf(out, size, p->back_key ? "Tap: do   Back: %s" : "Tap: do", close);
    else
        snprintf(out, size, "Enter: do   Up/Down: move   Esc: %s", close);
}

void snx_error_panel_draw(SnxErrorPanel* p, const SnxError* e, SnxUiList* l, int sw, int sh) {
    int items[SNX_ERROR_PANEL_ITEMS], n = page_items(p, e, items);
    int s = snx_menu_scale(sw, sh, p->percent), cols = snx_menu_cols(sw, s),
        finger = (FINGER_ROW * p->percent + 50) / 100;
    int ch = 16 * s, rh = p->fingers && finger > ch ? finger : ch, total = 0, msg_rows = 0, fixed;
    char name[256], keys[96], *message = NULL, *log = NULL, logline[1100];
    float w = (float)(cols * 8 * s), x, y;
    Pen pen = {NULL, 0, s, cols - 2};
    /* the game's name in capitals, as the window's title shows it */
    snprintf(name, sizeof name, "%s", e->game[0] ? e->game : "SCARLETNX");
    for (char* c = name; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    keys_line(p, e, keys, sizeof keys);
    /* the rows besides the message: head, name, a blank, the lines, a blank, the items, a blank, the keys */
    if (p->details) {
        message = fatal_shown(e->message[0] ? e->message : snx_error_line(e));
        log = e->log[0] ? fatal_shown(e->log) : NULL;
        snprintf(logline, sizeof logline, "Log: %s", log ? log : e->log);
        (void)para(&pen, 0, message ? message : e->message, 0, MESSAGE_ROWS, 0, &msg_rows);
        fixed = 1 + 1 + 1 + (log ? 1 + para(&pen, 0, logline, 0, LINE_ROWS, 0, NULL) : 0) + 1 + 1 + 1;
    } else
        fixed = 1 + 1 + 1 + para(&pen, 0, snx_error_line(e), 0, LINE_ROWS, 0, NULL) +
                para(&pen, 0, snx_error_hint(e), 0, LINE_ROWS, 0, NULL) + 1 + 1 + 1;
    if (p->said[0]) fixed += 1 + para(&pen, 0, p->said, 0, SAID_ROWS, 0, NULL);
    /* Details' message takes the rows the picture has left, three at the least */
    p->rows = msg_rows;
    p->rows_shown = 0;
    if (p->details) {
        int room = (sh - n * rh) / ch - fixed;
        p->rows_shown = msg_rows < (room < 3 ? 3 : room) ? msg_rows : (room < 3 ? 3 : room);
        if (p->top > msg_rows - p->rows_shown) p->top = msg_rows - p->rows_shown;
        if (p->top < 0) p->top = 0;
    }
    total = fixed + p->rows_shown;
    x = (float)((sw - cols * 8 * s) / 2);
    y = (float)((sh - total * ch - n * rh) / 2);
    if (y < 0) y = 0;
    p->box = (SnxErrorRect){x, y, w, (float)(total * ch + n * rh)};
    pen.l = l;
    pen.x = x + (float)(8 * s);
    ui_fill(l, x, y, w, p->box.h, UI_PAGE);
    ui_fill(l, x, y, w, (float)ch, UI_INK);
    ui_text(l, x + (float)(8 * s), y, s, p->details ? "SCARLETNX  /  DETAILS" : "SCARLETNX  /  STOPPED", UI_PAGE);
    y += (float)ch;
    text_at(l, pen.x, y, s, name, strlen(name), cols - 2, UI_INK);
    y += (float)(2 * ch);
    if (p->details) {
        y += (float)(para(&pen, y, message ? message : e->message, p->top, p->rows_shown, UI_INK, &msg_rows) * ch);
        /* more above or under: the font's arrows at the box's right */
        if (p->top) ui_text(l, x + w - (float)(8 * s), y - (float)(p->rows_shown * ch), s, "\x18", UI_INK);
        if (p->top + p->rows_shown < p->rows) ui_text(l, x + w - (float)(8 * s), y - (float)ch, s, "\x19", UI_INK);
        if (log) y += (float)((1 + para(&pen, y + (float)ch, logline, 0, LINE_ROWS, UI_INK, NULL)) * ch);
    } else {
        y += (float)(para(&pen, y, snx_error_line(e), 0, LINE_ROWS, UI_FAILURE, NULL) * ch);
        y += (float)(para(&pen, y, snx_error_hint(e), 0, LINE_ROWS, UI_INK, NULL) * ch);
    }
    if (p->said[0]) y += (float)((1 + para(&pen, y + (float)ch, p->said, 0, SAID_ROWS, UI_INK, NULL)) * ch);
    y += (float)ch;
    if (p->focus >= n) p->focus = n - 1;
    for (int i = 0; i < n; ++i) {
        const char* label = snx_error_action_label(items[i], e);
        char row[64];
        int on = i == p->focus;
        p->item[i] = (SnxErrorRect){x, y, w, (float)rh};
        p->action[i] = items[i];
        if (on) ui_fill(l, x, y, w, (float)rh, UI_INK);
        snprintf(row, sizeof row, "\xFE %s", label);
        ui_text(l, pen.x, y + (float)((rh - ch) / 2), s, row, on ? UI_PAGE : UI_INK);
        y += (float)rh;
    }
    p->items = n;
    text_at(l, pen.x, y + (float)ch, s, keys, strlen(keys), cols - 2, UI_INK);
    free(message);
    free(log);
}

/* the page's item i done: Details and Back change the page here; the rest is the caller's */
static int take(SnxErrorPanel* p, const SnxError* e, int i) {
    int items[SNX_ERROR_PANEL_ITEMS], n = page_items(p, e, items), action;
    if (i < 0 || i >= n) return SNX_ACTION_NONE;
    action = items[i];
    p->focus = i;
    if (action == SNX_ACTION_DETAILS || action == SNX_ACTION_BACK) {
        p->details = action == SNX_ACTION_DETAILS;
        p->top = 0;
        p->said[0] = 0;
        p->focus = 0;
        if (!p->details) { /* back on Details' own item */
            n = page_items(p, e, items);
            for (int k = 0; k < n; ++k)
                if (items[k] == SNX_ACTION_DETAILS) p->focus = k;
        }
        return SNX_ACTION_NONE;
    }
    return action;
}

int snx_error_panel_key(SnxErrorPanel* p, const SnxError* e, int key, int input, u64 now_ms) {
    int items[SNX_ERROR_PANEL_ITEMS], n = page_items(p, e, items);
    if (!p->at_once && now_ms < p->shown_ms + SNX_ERROR_PANEL_WAIT_MS) return SNX_ACTION_NONE;
    p->input = input;
    switch (key) {
        case SNX_ERROR_KEY_UP: p->focus = (p->focus + n - 1) % n; return SNX_ACTION_NONE;
        case SNX_ERROR_KEY_DOWN: p->focus = (p->focus + 1) % n; return SNX_ACTION_NONE;
        case SNX_ERROR_KEY_DO: return take(p, e, p->focus);
        case SNX_ERROR_KEY_BACK:
            if (!p->details) return SNX_ACTION_CLOSE;
            for (int i = 0; i < n; ++i)
                if (items[i] == SNX_ACTION_BACK) return take(p, e, i);
            return SNX_ACTION_NONE;
        case SNX_ERROR_KEY_PAGE_UP:
        case SNX_ERROR_KEY_PAGE_DOWN:
            snx_error_panel_scroll(p, (key == SNX_ERROR_KEY_PAGE_UP ? -1 : 1) *
                                          (p->rows_shown > 1 ? p->rows_shown - 1 : 1));
            return SNX_ACTION_NONE;
        default: return SNX_ACTION_NONE;
    }
}

static int hit(const SnxErrorPanel* p, float x, float y) {
    for (int i = 0; i < p->items; ++i)
        if (x >= p->item[i].x && x < p->item[i].x + p->item[i].w && y >= p->item[i].y &&
            y < p->item[i].y + p->item[i].h)
            return i;
    return -1;
}
int snx_error_panel_press(SnxErrorPanel* p, const SnxError* e, float x, float y, int down, int touch, u64 now_ms) {
    int at = hit(p, x, y), was = p->pressed;
    if (touch) p->input = SNX_ERROR_INPUT_TOUCH;
    if (down) {
        p->pressed = !p->at_once && now_ms < p->shown_ms + SNX_ERROR_PANEL_WAIT_MS ? -1 : at;
        return SNX_ACTION_NONE;
    }
    p->pressed = -1;
    return was >= 0 && at == was ? take(p, e, at) : SNX_ACTION_NONE;
}

void snx_error_panel_scroll(SnxErrorPanel* p, int rows) {
    int most = p->rows - p->rows_shown;
    p->top += rows;
    if (p->top > most) p->top = most;
    if (p->top < 0) p->top = 0;
}
