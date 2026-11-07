/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A skin for the on-screen controls (runtime/platform/vpad_skin.c): a RetroArch overlay this test writes, with a
   portrait page before its landscape one, a two-frame APNG, a D-pad of four directions, a stick with its ring and the
   descriptors a skin leaves out; one of several landscape pages, the one with the most controls read; then the
   shipped skins (argv[2]...), each with every control and its pressed look. argv[1] is a folder to write the test's
   skins in */
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/vpad_skin.h"
#include "nx/runtime/platform/controls_atlas.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ui.c's lock, without the platform layer */
#include "nx/runtime/platform/plat.h"
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* vpad.c's check of settings.txt's keys against its controls: a mismatch fails the test */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
/* settings.txt's Display table names options.c's setters */
void options_language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
static struct {
    char logged[16384]; /* every line logged since the last check */
    SnxUiQuad quads[512];
    SnxUiList list;
    char dir[1024]; /* where the test's skin is written */
    char path[2][1100];
    int which;
    u32 sprites[SNX_CONTROLS_ATLAS_WIDTH * SNX_CONTROLS_ATLAS_HEIGHT];
} g_test;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(g_test.logged);
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.logged + n, sizeof g_test.logged - n, fmt, ap);
    va_end(ap);
}
static int said(const char* text) { return strstr(g_test.logged, text) != NULL; }

static const int safe[4] = {0, 0, 2000, 1000};
static void draw(void) {
    g_test.list.q = g_test.quads;
    g_test.list.n = 0;
    g_test.list.max = 512;
    vpad_draw(&g_test.list, safe);
}
/* the quad drawn with those texels, or NULL */
static const SnxUiQuad* drawn(int u, int v) {
    int i;
    for (i = 0; i < g_test.list.n; ++i)
        if (g_test.quads[i].u == u && g_test.quads[i].v == v) return &g_test.quads[i];
    return NULL;
}
/* not near: a Windows header makes that a macro */
static int close_to(float a, float b) { return fabsf(a - b) < 0.001f; }
static u64 buttons(i32 sticks[4]) {
    u64 b;
    i32 s[4];
    vpad_state(&b, sticks ? sticks : s);
    return b;
}

/* ---- the test's images: plain colours, saved by SDL, and an APNG of two made from two of them */
static char* at(const char* name) { /* two at a time */
    g_test.which ^= 1;
    snprintf(g_test.path[g_test.which], sizeof g_test.path[0], "%s/%s", g_test.dir, name);
    return g_test.path[g_test.which];
}
static void png(const char* name, int w, int h, u32 rgba) { /* rgba: R G B A from the low byte */
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    int x, y;
    assert(s);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            u8* p = (u8*)s->pixels + y * s->pitch + x * 4;
            p[0] = (u8)rgba;
            p[1] = (u8)(rgba >> 8);
            p[2] = (u8)(rgba >> 16);
            p[3] = (u8)(rgba >> 24);
        }
    assert(SDL_SavePNG(s, at(name)));
    SDL_DestroySurface(s);
}
static u8* slurp(const char* name, size_t* n) {
    FILE* f = fopen(at(name), "rb");
    u8* p;
    assert(f);
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (u8*)malloc(*n);
    assert(p && fread(p, 1, *n, f) == *n);
    fclose(f);
    return p;
}
static u32 crc(const u8* p, size_t n) {
    u32 c = ~0u;
    size_t i;
    int b;
    for (i = 0; i < n; ++i)
        for (c ^= p[i], b = 0; b < 8; ++b) c = c & 1 ? c >> 1 ^ 0xEDB88320u : c >> 1;
    return ~c;
}
static void be(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static void put(FILE* f, const char* type, const u8* prefix, size_t np, const u8* data, size_t n) {
    u8 head[8], *all = (u8*)malloc(4 + np + n + 1);
    be(head, (u32)(np + n));
    memcpy(head + 4, type, 4);
    memcpy(all, type, 4);
    if (np) memcpy(all + 4, prefix, np);
    if (n) memcpy(all + 4 + np, data, n);
    be(head, (u32)(np + n));
    fwrite(head, 1, 8, f);
    fwrite(all + 4, 1, np + n, f);
    be(head, crc(all, 4 + np + n));
    fwrite(head, 1, 4, f);
    free(all);
}
/* name: an APNG of two frames, first's image then second's, each a PNG SDL saved (a frame's IDAT chunks) */
static void apng(const char* name, const char* first, const char* second) {
    size_t n0, n1, at0, at1;
    u8 *a = slurp(first, &n0), *b = slurp(second, &n1), ac[8], fc[26];
    FILE* f = fopen(at(name), "wb");
    u32 seq = 0;
    assert(f);
    fwrite(a, 1, 8, f);
    put(f, "IHDR", NULL, 0, a + 16, 13);
    be(ac, 2);
    be(ac + 4, 0);
    put(f, "acTL", NULL, 0, ac, 8);
    memset(fc, 0, sizeof fc);
    memcpy(fc + 4, a + 16, 8); /* the whole image */
    fc[21] = fc[23] = 1;       /* a delay of 1/1 s */
    be(fc, seq++);
    put(f, "fcTL", NULL, 0, fc, 26);
    for (at0 = 8; at0 < n0;
         at0 += 12 + ((size_t)a[at0] << 24 | (size_t)a[at0 + 1] << 16 | a[at0 + 2] << 8 | a[at0 + 3]))
        if (!memcmp(a + at0 + 4, "IDAT", 4))
            put(f, "IDAT", NULL, 0, a + at0 + 8,
                (size_t)a[at0] << 24 | (size_t)a[at0 + 1] << 16 | a[at0 + 2] << 8 | a[at0 + 3]);
    be(fc, seq++);
    put(f, "fcTL", NULL, 0, fc, 26);
    for (at1 = 8; at1 < n1;
         at1 += 12 + ((size_t)b[at1] << 24 | (size_t)b[at1 + 1] << 16 | b[at1 + 2] << 8 | b[at1 + 3]))
        if (!memcmp(b + at1 + 4, "IDAT", 4)) {
            u8 s[4];
            be(s, seq++);
            put(f, "fdAT", s, 4, b + at1 + 8,
                (size_t)b[at1] << 24 | (size_t)b[at1 + 1] << 16 | b[at1 + 2] << 8 | b[at1 + 3]);
        }
    put(f, "IEND", NULL, 0, NULL, 0);
    fclose(f);
    free(a);
    free(b);
}
static void text(const char* name, const char* s) {
    FILE* f = fopen(at(name), "wb");
    assert(f && fputs(s, f) >= 0);
    fclose(f);
}

/* the atlas's texel at u, v as R G B A from the low byte, premultiplied */
static u32 texel(int u, int v) {
    int w, h;
    u32 version;
    const u32* a;
    ui_atlas_set(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
    a = ui_atlas(&w, &h, &version);
    assert(a && u < w && v < h);
    return a[(size_t)v * (size_t)w + (size_t)u]; /* the test runs on a little-endian host: the bytes R G B A */
}

/* the skin at path, its controls put in as vpad_skin_apply does, without an atlas: 1 read, 0 not */
static int load(const char* path) {
    SnxSkinControl skin[SNX_VPAD_COUNT];
    int ok = vpad_skin_read(path, skin);
    vpad_skin_set(skin);
    return ok;
}
static void test_skin(void) {
    const SnxSkinControl *a, *b, *d, *st;
    const SnxUiQuad* q;
    i32 sticks[4];
    int i, w, h;
    u32 version;
    png("red.png", 40, 40, 0xFF0000FFu);
    png("green.png", 40, 40, 0xFF00FF00u);
    apng("a.png", "red.png", "green.png");
    png("b.png", 30, 60, 0xFFFF0000u);
    png("dir.png", 20, 40, 0xFF808080u);
    png("ring.png", 64, 64, 0x80FFFFFFu);
    png("knob.png", 64, 64, 0xFF00FFFFu);
    text("skin.cfg", "# a skin for tests/vpad_skin.c\n"
                     "overlays = 2\n"
                     "overlay0_name = \"portrait\"\n"
                     "overlay0_descs = 1\n"
                     "overlay0_desc0 = \"a,0.5,0.5,radial,0.1,0.1\"\n"
                     "overlay1_name = \"landscape\"\n"
                     "overlay1_normalized = true\n"
                     "overlay1_aspect_ratio = 2.0\n"
                     "overlay1_range_mod = 1.5\n"
                     "overlay1_alpha_mod = 2.0\n"
                     "overlay1_descs = 14\n"
                     "overlay1_desc0 = \"a,0.9,0.5,radial,0.05,0.1\"\n"
                     "overlay1_desc0_overlay = a.png\n"
                     "overlay1_desc1 = \"b,0.8,0.7,rect,0.05,0.1\"\n"
                     "overlay1_desc1_overlay = b.png\n"
                     "overlay1_desc1_reach_right = 2\n"
                     "overlay1_desc2 = \"up,0.2,0.4,rect,0.05,0.1\"\n"
                     "overlay1_desc2_overlay = dir.png\n"
                     "overlay1_desc3 = \"down,0.2,0.8,rect,0.05,0.1\"\n"
                     "overlay1_desc3_overlay = dir.png\n"
                     "overlay1_desc4 = \"left,0.1,0.6,rect,0.05,0.1\"\n"
                     "overlay1_desc4_overlay = dir.png\n"
                     "overlay1_desc5 = \"right,0.3,0.6,rect,0.05,0.1\"\n"
                     "overlay1_desc5_overlay = dir.png\n"
                     "overlay1_desc6 = \"left|up,0.1,0.4,rect,0.05,0.1\"\n"
                     "overlay1_desc7 = \"nul,0.5,0.5,radial,0.1,0.2\"\n"
                     "overlay1_desc7_overlay = ring.png\n"
                     "overlay1_desc8 = \"analog_left,0.5,0.5,radial,0.1,0.2\"\n"
                     "overlay1_desc8_overlay = knob.png\n"
                     "overlay1_desc8_movable = true\n"
                     "overlay1_desc8_saturate_pct = 0.5\n"
                     "overlay1_desc9 = \"overlay_next,0.5,0.1,rect,0.05,0.05\"\n"
                     "overlay1_desc10 = \"a,0.1,0.1,radial,0.05,0.1\"\n"
                     "overlay1_desc10_overlay = a.png\n"
                     "overlay1_desc11 = \"nul,0.7,0.2,rect,0.05,0.05\"\n"
                     "overlay1_desc11_overlay = ring.png\n"
                     "overlay1_desc12 = \"x,0.8,0.3,circle,0.05,0.1\"\n"
                     "overlay1_desc13 = \"y,0.7,0.5\"\n");
    /* B's place is in the game's .ini: it stays there; the others go where the skin puts them */
    assert(vpad_ini("controls", "1") && vpad_ini("controls_b", "0.250 0.100 100"));
    g_test.logged[0] = 0;
    assert(load(at("skin.cfg")));
    assert(said("overlay0 \"portrait\" is a portrait page: left out"));
    assert(said("overlay1 \"landscape\" has 4 of the 17 controls"));
    assert(said("overlay1 \"landscape\" is read: of its landscape pages, the first with the most controls (4)"));
    assert(said("overlay1_desc6 left|up is none of the controls: left out"));
    assert(said("overlay1_desc9 overlay_next is none of the controls: left out"));
    assert(said("overlay1 a again: left out, its first one is the control"));
    assert(said("overlay1's nul at 0.700 0.200 is no stick's ring: left out"));
    assert(said("overlay1_desc12 x has the hit area circle, not radial or rect: left out"));
    assert(said("overlay1_desc13 = \"y,0.7,0.5\" is not name,x,y,shape,range_x,range_y: left out"));
    assert(said("has no Home: the built-in one") && said("has no X: the built-in one"));
    assert(said("page 1 \"landscape\": 5 images"));

    /* A: its middle, a box of the shorter side's fifth (0.05 of the page across, which is twice as wide), round, the
       page's range_mod; its APNG's two frames */
    a = vpad_skin(SNX_VPAD_A);
    assert(a && close_to(a->x, 0.9f) && close_to(a->y, 0.5f) && close_to(a->w, 0.2f) && close_to(a->h, 0.2f) &&
           a->round);
    assert(close_to(a->reach[0], 1.5f) && close_to(a->reach[3], 1.5f) && a->pieces == 1 && a->piece[0].uw == 40);
    assert(a->piece[0].pu != a->piece[0].u || a->piece[0].pv != a->piece[0].v);
    assert(a->piece[0].v >= ui_skin_y() && a->piece[0].pv >= ui_skin_y());
    assert(close_to(vpad_config()->at[SNX_VPAD_A].x, 0.9f) && close_to(vpad_config()->at[SNX_VPAD_A].y, 0.5f));
    b = vpad_skin(SNX_VPAD_B);
    assert(b && !b->round && close_to(b->reach[1], 3) && close_to(b->reach[0], 1.5f) &&
           b->piece[0].pu == b->piece[0].u);
    assert(close_to(vpad_config()->at[SNX_VPAD_B].x, 0.25f)); /* the .ini's place */
    assert(!vpad_skin(SNX_VPAD_HOME) && !vpad_skin(SNX_VPAD_X) && !vpad_skin(SNX_VPAD_Y));
    /* the D-pad: the box around its four directions, each lit by its own */
    d = vpad_skin(SNX_VPAD_DPAD);
    assert(d && d->pieces == 4 && close_to(d->x, 0.2f) && close_to(d->y, 0.6f) && close_to(d->w, 0.6f) &&
           close_to(d->h, 0.6f));
    assert(d->piece[0].lit == 1u << 13 && close_to(d->piece[0].x, 1.0f / 3) && close_to(d->piece[0].y, 0) &&
           close_to(d->piece[0].w, 1.0f / 3) && close_to(d->piece[0].h, 1.0f / 3));
    /* the stick: its ring, then the knob that moves; pushed all the way at half its ring */
    st = vpad_skin(SNX_VPAD_STICK_L);
    assert(st && st->pieces == 2 && st->knob == 1 && !st->piece[0].lit && close_to(st->saturate, 0.5f));
    assert(close_to(st->w, 0.4f) && close_to(st->h, 0.4f));

    /* the texels: A's frames red then green, premultiplied, in the rows under the picture */
    assert(texel(a->piece[0].u + 20, a->piece[0].v + 20) == 0xFF0000FFu);
    assert(texel(a->piece[0].pu + 20, a->piece[0].pv + 20) == 0xFF00FF00u);
    assert(texel(st->piece[0].u + 5, st->piece[0].v + 5) == 0x80808080u);
    ui_atlas(&w, &h, &version);
    assert(w == SNX_CONTROLS_ATLAS_WIDTH && h > ui_skin_y() && h <= ui_skin_y() + ui_skin_room());

    /* A's hit area: round, half again its box (100 pixels from its middle on 1000): 140 across hits, 140 across and
       down is past the ellipse, which the box's corner would have taken */
    assert(vpad_down(1, 1800 + 140, 500, safe) && buttons(NULL) == 1 && vpad_up(1));
    buttons(NULL);
    assert(!vpad_down(2, 1800 + 140, 500 + 140, safe));
    /* drawn: A's frame, its pressed one held; B's one frame at its alpha, pressed at twice that */
    vpad_ini("controls_opacity", "50");
    draw();
    assert((q = drawn(a->piece[0].u, a->piece[0].v)) && close_to(q->w, 200) && close_to(q->x, 1700));
    assert(!drawn(a->piece[0].pu, a->piece[0].pv) && (q = drawn(b->piece[0].u, b->piece[0].v)) && q->rgba >> 24 == 127);
    assert(vpad_down(3, 1800, 500, safe) && vpad_down(4, 500, 100, safe));
    draw();
    assert(drawn(a->piece[0].pu, a->piece[0].pv) && !drawn(a->piece[0].u, a->piece[0].v));
    assert((q = drawn(b->piece[0].u, b->piece[0].v)) && q->rgba >> 24 == 254);
    assert(buttons(NULL) == 3 && vpad_up(3) && vpad_up(4));
    buttons(NULL);
    vpad_ini("controls_opacity", "100");
    /* the D-pad's up: its piece lit, the other three not */
    assert(vpad_down(5, 400, 600 - 250, safe) && buttons(NULL) == 1u << 13);
    vpad_ini("controls_opacity", "40");
    draw();
    for (i = 0; i < g_test.list.n; ++i)
        if (g_test.quads[i].u == d->piece[0].u && g_test.quads[i].v == d->piece[0].v)
            assert(g_test.quads[i].rgba >> 24 == (close_to(g_test.quads[i].y, 300) ? 204u : 102u));
    vpad_ini("controls_opacity", "100");
    assert(vpad_up(5));
    buttons(NULL);
    /* the stick: all the way at half its ring (100 pixels of 200), the knob there */
    assert(vpad_down(6, 1000, 500, safe));
    vpad_move(6, 1050, 500, safe);
    buttons(sticks);
    assert(sticks[0] > 16000 && sticks[0] < 16700);
    vpad_move(6, 1100, 500, safe);
    buttons(sticks);
    assert(sticks[0] == 32767);
    draw();
    assert((q = drawn(st->piece[1].pu, st->piece[1].pv)) && close_to(q->x, 800 + 100) && close_to(q->w, 400));
    assert(vpad_up(6));

    /* one that can't be read: the built-in look again, the places back with it, the atlas without the skin's rows */
    g_test.logged[0] = 0;
    assert(!load(at("none.cfg")) && said("none.cfg can't be read: the built-in look"));
    assert(!vpad_skin(SNX_VPAD_A) && close_to(vpad_config()->at[SNX_VPAD_A].x, 0.93f));
    texel(0, 0);
    ui_atlas(&w, &h, &version);
    assert(h == ui_skin_y());
    /* an image missing: none of the skin */
    text("broken.cfg", "overlays = 1\noverlay0_normalized = true\noverlay0_descs = 1\n"
                       "overlay0_desc0 = \"a,0.9,0.5,radial,0.05,0.1\"\noverlay0_desc0_overlay = gone.png\n");
    g_test.logged[0] = 0;
    assert(!load(at("broken.cfg")) && said("gone.png can't be read: the built-in look"));
    assert(!vpad_skin(SNX_VPAD_A));
}

/* a skin of several landscape pages, as libretro's retropad (a D-pad on one, sticks on others): the one that gives
   the most controls a look is read, the first of those on a tie; a portrait page is never one, and the pages weighed
   say nothing of what they leave out. A count of pages past what is weighed is said */
static void test_pages(void) {
    text("pages.cfg", "overlays = 5\n"
                      "overlay0_name = \"portrait\"\n"
                      "overlay0_normalized = true\n"
                      "overlay0_descs = 4\n"
                      "overlay0_desc0 = \"a,0.5,0.2,radial,0.05,0.1\"\noverlay0_desc0_overlay = red.png\n"
                      "overlay0_desc1 = \"b,0.5,0.4,radial,0.05,0.1\"\noverlay0_desc1_overlay = red.png\n"
                      "overlay0_desc2 = \"x,0.5,0.6,radial,0.05,0.1\"\noverlay0_desc2_overlay = red.png\n"
                      "overlay0_desc3 = \"y,0.5,0.8,radial,0.05,0.1\"\noverlay0_desc3_overlay = red.png\n"
                      "overlay1_name = \"landscape\"\n"
                      "overlay1_normalized = true\n"
                      "overlay1_descs = 3\n"
                      "overlay1_desc0 = \"a,0.1,0.5,radial,0.05,0.1\"\noverlay1_desc0_overlay = red.png\n"
                      "overlay1_desc1 = \"b,0.2,0.5,radial,0.05,0.1\"\n"
                      "overlay1_desc2 = \"a,0.3,0.5,radial,0.05,0.1\"\noverlay1_desc2_overlay = red.png\n"
                      "overlay2_name = \"landscape-analog\"\n"
                      "overlay2_normalized = true\n"
                      "overlay2_descs = 4\n"
                      "overlay2_desc0 = \"a,0.7,0.5,radial,0.05,0.1\"\noverlay2_desc0_overlay = red.png\n"
                      "overlay2_desc1 = \"b,0.8,0.6,radial,0.05,0.1\"\noverlay2_desc1_overlay = green.png\n"
                      "overlay2_desc2 = \"nul,0.3,0.5,radial,0.1,0.2\"\noverlay2_desc2_overlay = ring.png\n"
                      "overlay2_desc3 = \"analog_left,0.3,0.5,radial,0.1,0.2\"\n"
                      "overlay3_name = \"landscape-pixels\"\n"
                      "overlay3_descs = 4\n"
                      "overlay3_desc0 = \"a,900,500,radial,50,100\"\noverlay3_desc0_overlay = red.png\n"
                      "overlay3_desc1 = \"b,800,600,radial,50,100\"\noverlay3_desc1_overlay = red.png\n"
                      "overlay3_desc2 = \"x,800,400,radial,50,100\"\noverlay3_desc2_overlay = red.png\n"
                      "overlay3_desc3 = \"y,700,500,radial,50,100\"\noverlay3_desc3_overlay = red.png\n"
                      "overlay4_name = \"landscape-tie\"\n"
                      "overlay4_normalized = true\n"
                      "overlay4_descs = 3\n"
                      "overlay4_desc0 = \"a,0.6,0.5,radial,0.05,0.1\"\noverlay4_desc0_overlay = red.png\n"
                      "overlay4_desc1 = \"x,0.6,0.3,radial,0.05,0.1\"\noverlay4_desc1_overlay = red.png\n"
                      "overlay4_desc2 = \"y,0.5,0.4,radial,0.05,0.1\"\noverlay4_desc2_overlay = red.png\n");
    g_test.logged[0] = 0;
    assert(load(at("pages.cfg")));
    assert(said("overlay0 \"portrait\" is a portrait page: left out"));
    assert(said("overlay1 \"landscape\" has 1 of the 17 controls"));        /* B has no image, A counts once */
    assert(said("overlay2 \"landscape-analog\" has 3 of the 17 controls")); /* the stick by its ring */
    assert(said("overlay3 \"landscape-pixels\" has 0 of the 17 controls"));
    assert(said("overlay4 \"landscape-tie\" has 3 of the 17 controls"));
    assert(said("overlay2 \"landscape-analog\" is read: of its landscape pages, the first with the most controls (3)"));
    assert(!said("overlay3_desc0") && !said("overlay1_desc1") && !said("overlay1 a again"));
    assert(said("page 2 \"landscape-analog\": 3 images"));
    assert(vpad_skin(SNX_VPAD_A) && close_to(vpad_skin(SNX_VPAD_A)->x, 0.7f) && vpad_skin(SNX_VPAD_B));
    assert(vpad_skin(SNX_VPAD_STICK_L) && vpad_skin(SNX_VPAD_STICK_L)->pieces == 1);
    assert(!vpad_skin(SNX_VPAD_X) && !vpad_skin(SNX_VPAD_Y));
    /* more pages than are weighed: the first of them */
    text("many.cfg", "overlays = 4000000000\n"
                     "overlay0_normalized = true\n"
                     "overlay0_descs = 1\n"
                     "overlay0_desc0 = \"a,0.4,0.5,radial,0.05,0.1\"\noverlay0_desc0_overlay = red.png\n");
    g_test.logged[0] = 0;
    assert(load(at("many.cfg")) && said("has 4000000000 pages (overlays): the first 64 weighed"));
    assert(said("overlay63 \"\" has 0 of the 17 controls") && !said("overlay64"));
    assert(said("overlay0 \"\" is read: of its landscape pages, the first with the most controls (1)"));
    assert(close_to(vpad_skin(SNX_VPAD_A)->x, 0.4f));
    /* a page of more descriptors than are read is weighed by those read */
    text("descs.cfg", "overlays = 2\n"
                      "overlay0_normalized = true\n"
                      "overlay0_descs = 1\n"
                      "overlay0_desc0 = \"a,0.4,0.5,radial,0.05,0.1\"\noverlay0_desc0_overlay = red.png\n"
                      "overlay1_normalized = true\n"
                      "overlay1_descs = 4294967295\n"
                      "overlay1_desc0 = \"a,0.6,0.5,radial,0.05,0.1\"\noverlay1_desc0_overlay = red.png\n"
                      "overlay1_desc1 = \"b,0.7,0.5,radial,0.05,0.1\"\noverlay1_desc1_overlay = red.png\n");
    g_test.logged[0] = 0;
    assert(load(at("descs.cfg")) && said("overlay1 \"\" has 2 of the 17 controls"));
    assert(close_to(vpad_skin(SNX_VPAD_A)->x, 0.6f) && vpad_skin(SNX_VPAD_B));
    vpad_skin_set(NULL);
}

/* controls_skin while the game runs, as the game's menu and a reload set it: the backend sees it changed once, and
   applying it makes the atlas again with the skin's rows and puts the controls in with it; a finger on a control lets
   go. Empty is the built-in look; a name ScarletNX doesn't ship, and no path, is said and the built-in look */
static void test_switch(const char* tests_skin) {
    const IniOpt* o = ini_find(g_vpad_ini, "controls_skin");
    int w, h, yorha_h;
    u32 version, before;
    char name[64];
    assert(o && o->type == INI_TEXT && !g_settings.controls_skin[0] && !vpad_skin_changed());
    assert(vpad_skin_shipped(0, name, sizeof name) && !strcmp(name, "yorha") &&
           !vpad_skin_shipped(1, name, sizeof name));
    /* the shipped one, by its name: read from the files compiled in */
    assert(vpad_ini("controls_skin", "yorha") && vpad_skin_changed() && !vpad_skin_changed());
    ui_atlas(&w, &h, &before);
    g_test.logged[0] = 0;
    assert(vpad_skin_apply(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT));
    assert(said("vpad skin: yorha, page 0 \"landscape\": 20 images in"));
    ui_atlas(&w, &h, &version);
    assert(version == before + 1 && h > ui_skin_y() + 1000);
    yorha_h = h;
    assert(vpad_skin(SNX_VPAD_A) && vpad_skin(SNX_VPAD_STICK_L) && vpad_skin(SNX_VPAD_DPAD)->pieces == 4);
    assert(close_to(vpad_config()->at[SNX_VPAD_A].x, 0.93f) && vpad_skin(SNX_VPAD_A)->round);
    /* held while it changes: let go */
    assert(vpad_down(1, 1860, 470, safe) && buttons(NULL) == 1);
    assert(vpad_ini("controls_skin", tests_skin) && vpad_skin_changed());
    assert(vpad_skin_apply(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT));
    assert(!buttons(NULL) && !vpad_up(1));
    ui_atlas(&w, &h, &version);
    assert(version == before + 2 && h < yorha_h && h > ui_skin_y());
    assert(close_to(vpad_config()->at[SNX_VPAD_A].x, 0.9f) && !vpad_skin(SNX_VPAD_X));
    /* empty: the built-in look, the atlas without rows of a skin */
    assert(vpad_ini("controls_skin", "") && vpad_skin_changed());
    assert(vpad_skin_apply(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT));
    ui_atlas(&w, &h, &version);
    assert(h == ui_skin_y() && !vpad_skin(SNX_VPAD_A) && close_to(vpad_config()->at[SNX_VPAD_A].x, 0.93f));
    /* a name ScarletNX doesn't ship */
    g_test.logged[0] = 0;
    assert(vpad_ini("controls_skin", "leafy") && vpad_skin_changed());
    assert(!vpad_skin_apply(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT));
    assert(said("controls_skin=leafy is neither a skin ScarletNX has (yorha) nor the path of a RetroArch overlay's "
                ".cfg: the built-in look"));
    assert(!vpad_skin(SNX_VPAD_A));
    /* no atlas made (no built-in image): no skin's controls either, they would draw from rows it lacks */
    assert(vpad_ini("controls_skin", "yorha") && vpad_skin_changed());
    assert(!vpad_skin_apply(NULL, 0, 0) && !vpad_skin(SNX_VPAD_A));
    assert(vpad_ini("controls_skin", "") && vpad_skin_changed());
    vpad_skin_apply(g_test.sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
    vpad_ini("controls_b", "0.870 0.600 100");
}

/* a shipped skin: every control from it, each image with its pressed look */
static void test_shipped(const char* path) {
    int c, i;
    g_test.logged[0] = 0;
    if (!load(path)) {
        fprintf(stderr, "%s", g_test.logged);
        assert(!"a shipped skin loads");
    }
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        const SnxSkinControl* k = vpad_skin(c);
        assert(k);
        for (i = 0; i < k->pieces; ++i)
            if (k->piece[i].lit) assert(k->piece[i].pu != k->piece[i].u || k->piece[i].pv != k->piece[i].v);
    }
    assert(!said("left out"));
    vpad_skin_set(NULL);
}

int main(int argc, char** argv) {
    int i;
    assert(argc >= 2);
    snprintf(g_test.dir, sizeof g_test.dir, "%s", argv[1]);
    SDL_CreateDirectory(g_test.dir);
    test_skin();
    test_pages();
    test_switch(at("skin.cfg"));
    for (i = 2; i < argc; ++i) test_shipped(argv[i]);
    printf(
        "vpad_skin: a RetroArch overlay's controls, images, hit areas and pressed looks, and %d shipped skins passed\n",
        argc - 2);
    return 0;
}
