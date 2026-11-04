/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/stats.h"
#include <stdio.h>
#include <string.h>

#define STATS_SPAN_NS 500000000ull /* the frame rate is over the last half second */
#define STATS_LINES   2

void snx_stats_init(SnxStats* s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->fps10 = s->gen10 = -1;
}

void snx_stats_frame(SnxStats* s, uint64_t now_ns) {
    uint64_t span;
    if (!s || !snx_stats_visible(s)) return;
    s->prev_ns = s->last_ns;
    s->last_ns = now_ns;
    ++s->frames;
    if (!s->since_ns) { /* the first half second starts at the first frame */
        s->since_ns = now_ns;
        s->since = s->frames;
        s->since_gen = s->generated;
        return;
    }
    span = now_ns - s->since_ns;
    if (span < STATS_SPAN_NS) return;
    s->fps10 = (int)(((s->frames - s->since) * 10000000000ull + span / 2) / span);
    s->gen10 = (int)(((s->generated - s->since_gen) * 10000000000ull + span / 2) / span);
    s->since_ns = now_ns;
    s->since = s->frames;
    s->since_gen = s->generated;
}

void snx_stats_generated(SnxStats* s) {
    if (snx_stats_visible(s)) ++s->generated;
}

int snx_stats_toggle(SnxStats* s, int showcase) {
    int on, keep_on, keep_showcase;
    if (!s) return 0;
    on = showcase ? !s->showcase : !s->on;
    keep_on = s->on;
    keep_showcase = s->showcase;
    snx_stats_init(s);
    s->on = keep_on;
    s->showcase = keep_showcase;
    if (showcase)
        s->showcase = on;
    else
        s->on = on;
    return on;
}

int snx_stats_visible(const SnxStats* s) { return s && (s->on || s->showcase); }

void snx_stats_line(SnxStats* s, uint64_t now_ns, char* out, size_t size) {
    char fps[16], ms[16];
    (void)now_ns;
    if (!s || !out || !size) return;
    if (s->fps10 >= 0)
        snprintf(fps, sizeof fps, "%d.%d fps", s->fps10 / 10, s->fps10 % 10);
    else
        snprintf(fps, sizeof fps, "? fps");
    if (s->prev_ns && s->last_ns > s->prev_ns) {
        int tenths = (int)(((s->last_ns - s->prev_ns) + 50000u) / 100000u);
        snprintf(ms, sizeof ms, "%d.%d ms", tenths / 10, tenths % 10);
    } else
        snprintf(ms, sizeof ms, "? ms");
    if (s->gen10 > 0) {
        int total = s->fps10 + s->gen10;
        snprintf(out, size, "%s  %d.%d gen  %d.%d total  %s", fps, s->gen10 / 10, s->gen10 % 10, total / 10, total % 10,
                 ms);
    } else
        snprintf(out, size, "%s  %s", fps, ms);
}

void snx_stats_inputs(char* out, size_t size, uint32_t held, const SnxStatsButton* buttons, unsigned count) {
    size_t at = 0;
    unsigned i;
    if (!out || !size) return;
    out[0] = 0;
    for (i = 0; buttons && i < count; ++i) {
        size_t n;
        if (!(held & buttons[i].bit)) continue;
        n = strlen(buttons[i].name);
        if (at + n + 2 > size) break;
        if (at) out[at++] = ' ';
        memcpy(out + at, buttons[i].name, n);
        at += n;
        out[at] = 0;
    }
    if (!at) snprintf(out, size, "-");
}

int snx_stats_draw(SnxText* text, SnxStats* s, VkCommandBuffer cb, VkImageView view, VkFormat format, uint32_t width,
                   uint32_t height, float white, uint32_t slot, uint64_t now_ns, const char* inputs) {
    const char* lines[STATS_LINES];
    char rate[80];
    unsigned widest, longest, count = STATS_LINES;
    int scale;
    float x, y;
    if (!text || !snx_stats_visible(s) || !width || !height) return 0;
    snx_stats_line(s, now_ns, rate, sizeof rate);
    lines[0] = rate;
    lines[1] = inputs && inputs[0] ? inputs : NULL;
    widest = (unsigned)strlen(rate);
    longest = lines[1] ? (unsigned)strlen(lines[1]) : 0;
    if (longest > widest) widest = longest;
    if (!lines[1]) count = 1;
    /* two pixels a font pixel where the target is tall enough, one where the lines would not fit */
    scale = height >= 400 ? 2 : 1;
    while (scale > 1 && (widest * 8u * (unsigned)scale) + 16u * (unsigned)scale > width) --scale;
    x = 8.0f * (float)scale;
    y = (float)height - (float)(count * 16 * (unsigned)scale) - 12.0f * (float)scale;
    return snx_text_draw(text, cb, view, format, width, height, white, slot, x, y, scale, lines, count);
}
