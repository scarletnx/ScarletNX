/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/vt_pages.h"

static u32 level_dim(u32 d, u32 l) { return d >> l ? d >> l : 1; }

int vt_shape(VtShape* s, u32 w, u32 h, u32 layers, u32 levels, u32 gw, u32 gh, u32 device_tail) {
    if (!w || !h || !layers || !levels || !gw || !gh || levels > VT_LEVELS) return 0;
    *s = (VtShape){.w = w, .h = h, .layers = layers, .levels = levels, .gw = gw, .gh = gh};
    if (device_tail > levels) device_tail = levels;
    s->tail_first = device_tail;
    u32 at = 0;
    for (u32 l = 0; l < device_tail; l++) {
        s->first[l] = at;
        s->bx[l] = (level_dim(w, l) + gw - 1) / gw;
        s->by[l] = (level_dim(h, l) + gh - 1) / gh;
        at += s->bx[l] * s->by[l];
        if (s->tail_first == device_tail && (level_dim(w, l) < gw || level_dim(h, l) < gh)) s->tail_first = l;
    }
    s->first[device_tail] = at;
    s->per_layer = at;
    return 1;
}
int vt_block_pinned(const VtShape* s, u32 b) { return s->per_layer && b % s->per_layer >= s->first[s->tail_first]; }

/* v into list of n, once */
static u32 add_once(u32* list, u32 n, u32 cap, u32 v) {
    for (u32 i = 0; i < n; i++)
        if (list[i] == v) return n;
    if (n < cap) list[n++] = v;
    return n;
}
/* An axis of a mark: the blocks (columns or rows) of a level `size` texels long, in blocks of g, that the cell lies
   in (under) and that a filter VT_REACH texels wide reaches from it (near, those of under left out). A coordinate
   outside 0 to 1 has three candidate cells: the wrapped one, the mirrored one, and the edge it left by */
#define AXIS_MAX 16u
static void axis_blocks(u32 size, u32 g, u32 cell, u32 outside, u32* under, u32* nunder, u32* near, u32* nnear) {
    u32 cells[3] = {cell, VT_CELLS - 1 - cell, outside == 1 ? 0 : VT_CELLS - 1};
    const u32 ncells = outside ? 3 : 1, last = (size - 1) / g;
    *nunder = *nnear = 0;
    for (u32 k = 0; k < ncells; k++) {
        /* the cell's texels: [lo, hi] */
        const u32 lo = (u32)((u64)cells[k] * size / VT_CELLS);
        u32 hi = (u32)(((u64)(cells[k] + 1) * size + VT_CELLS - 1) / VT_CELLS);
        hi = hi ? hi - 1 : 0;
        if (hi > size - 1) hi = size - 1;
        if (hi < lo) hi = lo;
        for (u32 b = lo / g; b <= hi / g; b++) *nunder = add_once(under, *nunder, AXIS_MAX, b);
    }
    for (u32 k = 0; k < ncells; k++) {
        const u32 lo = (u32)((u64)cells[k] * size / VT_CELLS);
        u32 hi = (u32)(((u64)(cells[k] + 1) * size + VT_CELLS - 1) / VT_CELLS);
        hi = hi ? hi - 1 : 0;
        if (hi > size - 1) hi = size - 1;
        if (hi < lo) hi = lo;
        /* the reach each way, cut at the level's edges and carried on from the other edge */
        const u32 reach = VT_REACH < size ? VT_REACH : size - 1;
        const u32 first = lo >= reach ? lo - reach : 0, end = hi + reach < size ? hi + reach : size - 1;
        u32 list[AXIS_MAX], n = 0;
        for (u32 b = first / g; b <= end / g; b++) n = add_once(list, n, AXIS_MAX, b);
        if (lo < reach)
            for (u32 b = (size - (reach - lo)) / g; b <= last; b++) n = add_once(list, n, AXIS_MAX, b);
        if (hi + reach > size - 1)
            for (u32 b = 0; b <= (hi + reach - size) / g; b++) n = add_once(list, n, AXIS_MAX, b);
        for (u32 i = 0; i < n; i++) {
            u32 in_under = 0;
            for (u32 j = 0; j < *nunder; j++) in_under |= under[j] == list[i];
            if (!in_under) *nnear = add_once(near, *nnear, AXIS_MAX, list[i]);
        }
    }
}
void vt_mark_blocks(const VtShape* s, const VtMark* m, u32* under, u32* nunder, u32* near, u32* nnear) {
    *nunder = *nnear = 0;
    if (m->level >= s->tail_first || m->layer >= s->layers || m->u >= VT_CELLS || m->v >= VT_CELLS) return;
    u32 xu[AXIS_MAX], xn[AXIS_MAX], yu[AXIS_MAX], yn[AXIS_MAX], nxu, nxn, nyu, nyn;
    axis_blocks(level_dim(s->w, m->level), s->gw, m->u, m->uo, xu, &nxu, xn, &nxn);
    axis_blocks(level_dim(s->h, m->level), s->gh, m->v, m->vo, yu, &nyu, yn, &nyn);
    const u32 base = m->layer * s->per_layer + s->first[m->level], across = s->bx[m->level];
    for (u32 j = 0; j < nyu; j++)
        for (u32 i = 0; i < nxu; i++) *nunder = add_once(under, *nunder, VT_MARK_BLOCKS, base + yu[j] * across + xu[i]);
    /* near: every other pair of a column and a row the filter reaches */
    for (u32 j = 0; j < nyu + nyn; j++)
        for (u32 i = 0; i < nxu + nxn; i++) {
            if (j < nyu && i < nxu) continue;
            const u32 y = j < nyu ? yu[j] : yn[j - nyu], x = i < nxu ? xu[i] : xn[i - nxu];
            *nnear = add_once(near, *nnear, VT_MARK_BLOCKS, base + y * across + x);
        }
}

u32 vt_block_parent(const VtShape* s, u32 b) {
    const u32 layer = b / s->per_layer, in = b % s->per_layer;
    u32 l = 0;
    while (l + 1 < s->tail_first && in >= s->first[l + 1]) l++;
    if (layer >= s->layers || l + 1 >= s->tail_first) return ~0u;
    const u32 k = in - s->first[l], x = k % s->bx[l], y = k / s->bx[l];
    /* the block's middle texel, halved into the next level */
    const u32 lw = level_dim(s->w, l), lh = level_dim(s->h, l);
    u32 mx = x * s->gw + s->gw / 2, my = y * s->gh + s->gh / 2;
    if (mx > lw - 1) mx = lw - 1;
    if (my > lh - 1) my = lh - 1;
    u32 px = mx / 2 / s->gw, py = my / 2 / s->gh;
    if (px > s->bx[l + 1] - 1) px = s->bx[l + 1] - 1;
    if (py > s->by[l + 1] - 1) py = s->by[l + 1] - 1;
    return layer * s->per_layer + s->first[l + 1] + py * s->bx[l + 1] + px;
}

void vt_age_count(u32 hist[VT_AGES], const u32* seen, const u8* held, u32 n, u32 now) {
    for (u32 b = 0; b < n; b++) {
        if (!(held[b >> 3] >> (b & 7) & 1)) continue;
        const u32 age = seen[b] && now >= seen[b] ? now - seen[b] : VT_AGES - 1;
        hist[age < VT_AGES - 1 ? age : VT_AGES - 1]++;
    }
}
u32 vt_window_next(u32 window, u32 base, u64 asked, u64 back, int pressed) {
    if (base > VT_AGES - 1) base = VT_AGES - 1;
    if (pressed || window < base) return base;
    if (asked >= 32 && back * 8 > asked) return window * 2 < VT_AGES - 1 ? window * 2 : VT_AGES - 1;
    if (back * 32 < asked || !asked) return window - window / 8 > base ? window - window / 8 : base;
    return window;
}
u32 vt_age_cut(const u32 hist[VT_AGES], u64 keep, u32 floor) {
    u64 kept = 0;
    u32 age = 0;
    while (age < VT_AGES && (age < floor || kept + hist[age] <= keep)) kept += hist[age++];
    return age;
}
