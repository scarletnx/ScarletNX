/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel (common/utilities/error_panel.c): laid out within the picture from a GBA's window to a phone's, the
   action that fixes the error first and in focus, the line in the failure colour and the rest in the ink (3:1 at the
   least on the page), the foot naming the keys of the input used last; no input before its wait; Up, Down, do and back,
   Details and its scroll, a press that counts only where it went down and came up; and its quads as rectangles */
#include "common/utilities/error_panel.h"
#include "common/utilities/notice.h"
#include "common/utilities/ui_colors.h"
#include "common/utilities/ui_rects.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SnxUiQuad g_quads[8192];

/* the letters drawn on the row whose top is y, left to right, as the font's bytes */
static void row_text(const SnxUiList* l, float y, char* out, size_t size) {
    float xs[256];
    char cs[256];
    int n = 0;
    for (int i = 0; i < l->n && n < 255; ++i) {
        SnxUiQuadView q = ui_view(&l->q[i]);
        if (q.ink != UI_GLYPH || q.y < y - 0.5f || q.y > y + 0.5f) continue;
        xs[n] = q.x;
        cs[n++] = (char)q.glyph;
    }
    for (int i = 1; i < n; ++i) /* by x */
        for (int k = i; k > 0 && xs[k - 1] > xs[k]; --k) {
            float tx = xs[k];
            char tc = cs[k];
            xs[k] = xs[k - 1], cs[k] = cs[k - 1];
            xs[k - 1] = tx, cs[k - 1] = tc;
        }
    size_t m = 0;
    for (int i = 0; i < n && m + 2 < size; ++i) {
        if (i && xs[i] - xs[i - 1] > 9.0f * (xs[1] - xs[0] > 0 ? (xs[1] - xs[0]) / 8.0f : 1.0f)) out[m++] = ' ';
        out[m++] = cs[i];
    }
    out[m] = 0;
}
/* whether a row of letters says text, the spaces between words left out */
static int says(const SnxUiList* l, float y, const char* text) {
    char row[300], want[300];
    size_t m = 0;
    row_text(l, y, row, sizeof row);
    for (const char* c = text; *c && m + 1 < sizeof want; ++c)
        if (*c != ' ') want[m++] = *c;
    want[m] = 0;
    m = 0;
    for (char* c = row; *c; ++c)
        if (*c != ' ') row[m++] = *c;
    row[m] = 0;
    return strstr(row, want) != NULL;
}
static int colour_used(const SnxUiList* l, u32 rgba) {
    for (int i = 0; i < l->n; ++i)
        if (ui_view(&l->q[i]).ink == UI_GLYPH && ui_view(&l->q[i]).rgba == ui_rgba(rgba)) return 1;
    return 0;
}
static SnxUiList draw(SnxErrorPanel* p, const SnxError* e, int w, int h) {
    SnxUiList l = {g_quads, 0, (int)(sizeof g_quads / sizeof *g_quads)};
    snx_error_panel_draw(p, e, &l, w, h);
    return l;
}
/* the foot's row: under the last item, a blank row between */
static float foot_y(const SnxErrorPanel* p, int w, int h) {
    const SnxErrorRect* last = &p->item[p->items - 1];
    return last->y + last->h + 16.0f * (float)snx_menu_scale(w, h, p->percent);
}
/* an item's letters' row, in the middle of its own */
static float label_y(const SnxErrorPanel* p, int i, int w, int h) {
    return p->item[i].y + (p->item[i].h - 16.0f * (float)snx_menu_scale(w, h, p->percent)) / 2.0f;
}

int main(void) {
    static const int sizes[][3] = {{720, 480, 100}, {1280, 720, 100}, {3840, 2160, 200}, {2400, 1080, 262}};
    SnxError e = {.kind = SNX_ERROR_SETTING,
                  .key = "hooks",
                  .line = "Native hooks is off.",
                  .game = "A Game",
                  .program = "/g/G",
                  .log = "/home/someone/g/G.log",
                  .message = "sm: the program opened pctl, which the runtime answers only in the SDK's own functions"};
    SnxErrorPanel p = {0};
    SnxUiList l;
    /* every size: the box within the picture, an item a row, the fix first and in focus, the name and the line */
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; ++k) {
        int w = sizes[k][0], h = sizes[k][1];
        p = (SnxErrorPanel){.percent = sizes[k][2], .fingers = sizes[k][2] > 200, .back_key = sizes[k][2] > 200};
        snx_error_panel_start(&p, 1000);
        l = draw(&p, &e, w, h);
        assert(p.box.x >= 0 && p.box.y >= 0 && p.box.x + p.box.w <= (float)w && p.box.y + p.box.h <= (float)h);
        assert(p.items == 5 && p.action[0] == SNX_ACTION_OPEN_SETTINGS && p.focus == 0);
        assert(p.action[3] == SNX_ACTION_DETAILS && p.action[4] == SNX_ACTION_CLOSE);
        for (int i = 0; i < p.items; ++i)
            assert(p.item[i].y >= p.box.y && p.item[i].y + p.item[i].h <= p.box.y + p.box.h &&
                   (i == 0 || p.item[i].y >= p.item[i - 1].y + p.item[i - 1].h));
        if (p.fingers) assert(p.item[0].h >= 48.0f * (float)p.percent / 100.0f - 1);
        /* the line in the failure colour, the rest in the ink: the soft grey is under 3:1 on the page */
        assert(colour_used(&l, UI_FAILURE) && colour_used(&l, UI_INK) && !colour_used(&l, UI_SOFT));
        assert(says(&l, label_y(&p, 0, w, h), "Open Settings") && says(&l, label_y(&p, 4, w, h), "Close"));
        assert(says(&l, p.box.y + 16.0f * (float)snx_menu_scale(w, h, p.percent), "A GAME"));
        /* its quads as rectangles: some, each on the picture */
        {
            SnxUiRects rects = {0};
            assert(snx_ui_rects(&rects, &l, w, h) && rects.count > 0);
            for (size_t i = 0; i < rects.count; ++i)
                assert(rects.rect[i].x >= 0 && rects.rect[i].y >= 0 && rects.rect[i].x + rects.rect[i].w <= w &&
                       rects.rect[i].y + rects.rect[i].h <= h);
            snx_ui_rects_free(&rects);
        }
    }
    /* the foot: the keys of the input used last */
    p = (SnxErrorPanel){.percent = 100};
    snx_error_panel_start(&p, 1000);
    l = draw(&p, &e, 1280, 720);
    {
        char row[300];
        float y = foot_y(&p, 1280, 720);
        row_text(&l, y, row, sizeof row);
        if (!says(&l, y, "Enter: do   Up/Down: move   Esc: close")) fprintf(stderr, "the foot says \"%s\"\n", row);
        assert(says(&l, y, "Enter: do   Up/Down: move   Esc: close"));
        snprintf(p.pad_do, sizeof p.pad_do, "Cross");
        snprintf(p.pad_back, sizeof p.pad_back, "Circle");
        assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DOWN, SNX_ERROR_INPUT_PAD, 1600) == SNX_ACTION_NONE);
        l = draw(&p, &e, 1280, 720);
        assert(says(&l, y, "Cross: do   D-pad: move   Circle: close"));
        e.from_launcher = 1; /* the launcher behind: Close goes back to its menu */
        l = draw(&p, &e, 1280, 720);
        assert(says(&l, y, "Circle: menu") && p.action[p.items - 1] == SNX_ACTION_CLOSE);
        e.from_launcher = 0;
    }
    /* no input before its wait: a press meant for the game takes no action */
    p = (SnxErrorPanel){.percent = 100};
    snx_error_panel_start(&p, 1000);
    l = draw(&p, &e, 1280, 720);
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DO, SNX_ERROR_INPUT_KEYS, 1000 + SNX_ERROR_PANEL_WAIT_MS - 1) ==
           SNX_ACTION_NONE);
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DOWN, SNX_ERROR_INPUT_KEYS, 1200) == SNX_ACTION_NONE &&
           p.focus == 0);
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DO, SNX_ERROR_INPUT_KEYS, 1500) == SNX_ACTION_OPEN_SETTINGS);
    /* Up from the first goes round to the last; back from the main page closes */
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_UP, SNX_ERROR_INPUT_KEYS, 1600) == SNX_ACTION_NONE &&
           p.focus == p.items - 1);
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_BACK, SNX_ERROR_INPUT_KEYS, 1600) == SNX_ACTION_CLOSE);
    /* Details: its own page, its items; back to the main page, Details in focus */
    p.focus = 3;
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DO, SNX_ERROR_INPUT_KEYS, 1700) == SNX_ACTION_NONE && p.details);
    p.open_log = 1;
    l = draw(&p, &e, 1280, 720);
    assert(p.items == 3 && p.action[0] == SNX_ACTION_OPEN_LOG && p.action[1] == SNX_ACTION_COPY_TEXT &&
           p.action[2] == SNX_ACTION_BACK);
    assert(says(&l, p.box.y + 16.0f * 2 * 3, "sm: the program opened pctl"));
    {
        int seen = 0; /* the log's place, its home folder as ~ */
        for (float y = p.box.y; y < p.box.y + p.box.h; y += 32) seen |= says(&l, y, "Log: ~/g/G.log");
        assert(seen);
    }
    assert(snx_error_panel_key(&p, &e, SNX_ERROR_KEY_BACK, SNX_ERROR_INPUT_KEYS, 1800) == SNX_ACTION_NONE &&
           !p.details && p.focus == 3);
    p.open_log = 0; /* a phone: no viewer, Share Log sends the log */
    p.focus = 3;
    (void)snx_error_panel_key(&p, &e, SNX_ERROR_KEY_DO, SNX_ERROR_INPUT_KEYS, 1900);
    l = draw(&p, &e, 1280, 720);
    assert(p.items == 2 && p.action[0] == SNX_ACTION_COPY_TEXT);
    /* a long message scrolls, within its ends */
    {
        SnxError lng = e;
        char* m = lng.message;
        for (size_t k = 0; k + 6 < sizeof lng.message; k += 5) memcpy(m + k, "word ", 5);
        m[sizeof lng.message - 1] = 0;
        p.top = 0;
        l = draw(&p, &lng, 720, 480);
        assert(p.rows > p.rows_shown && p.rows_shown >= 3);
        assert(snx_error_panel_key(&p, &lng, SNX_ERROR_KEY_PAGE_DOWN, SNX_ERROR_INPUT_KEYS, 2000) == SNX_ACTION_NONE &&
               p.top == (p.rows_shown - 1 < p.rows - p.rows_shown ? p.rows_shown - 1 : p.rows - p.rows_shown));
        snx_error_panel_scroll(&p, 100000);
        assert(p.top == p.rows - p.rows_shown);
        snx_error_panel_scroll(&p, -100000);
        assert(p.top == 0);
    }
    /* a press counts where it went down and came up, on an item, after the wait */
    p = (SnxErrorPanel){.percent = 100};
    snx_error_panel_start(&p, 1000);
    l = draw(&p, &e, 1280, 720);
    {
        float x = p.item[1].x + 4, y0 = p.item[0].y + 2, y1 = p.item[1].y + 2;
        assert(snx_error_panel_press(&p, &e, x, y1, 1, 1, 1100) == SNX_ACTION_NONE); /* in the wait */
        assert(snx_error_panel_press(&p, &e, x, y1, 0, 1, 1600) == SNX_ACTION_NONE);
        assert(snx_error_panel_press(&p, &e, x, y0, 1, 1, 1600) == SNX_ACTION_NONE);
        assert(snx_error_panel_press(&p, &e, x, y1, 0, 1, 1650) == SNX_ACTION_NONE); /* came up elsewhere */
        assert(snx_error_panel_press(&p, &e, x, y1, 1, 1, 1700) == SNX_ACTION_NONE);
        assert(snx_error_panel_press(&p, &e, x, y1, 0, 1, 1750) == p.action[1] && p.input == SNX_ERROR_INPUT_TOUCH);
        /* off the items: nothing */
        assert(snx_error_panel_press(&p, &e, 1, 1, 1, 0, 1800) == SNX_ACTION_NONE &&
               snx_error_panel_press(&p, &e, 1, 1, 0, 0, 1850) == SNX_ACTION_NONE);
    }
    puts("error_panel: ok");
    return 0;
}
