/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The controller applet's dialog (runtime/platform/ctrl_applet.c): the request read from arguments built here, in the
   layouts of each version, the sentences that explain it, the dialog's cards, gestures, answers and the result's bytes,
   the mouse notice, and its pictures drawn on the CPU (SNX_TEST_CTRL_SHOTS: a folder to save them in as PNG) */
#include "nx/runtime/platform/ctrl_applet.h"
#include "nx/runtime/platform/ctrl_scan.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include "support/ui_raster.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define ARG8 0x430
#define ARG4 0x21c
static u8 common[0x20], priv[0x14], arg[ARG8];
/* the common arguments of version v, the private argument (mode, styles, hold, and the argument's size it says) and
   an argument of the header's counts and flags, its colors and texts left to the caller */
static void build(u32 v, int mode, u32 styles, u32 hold, u32 says, int min, int max) {
    memset(common, 0, sizeof common);
    snx_st_le32(common, 1);
    snx_st_le32(common + 4, sizeof common);
    snx_st_le32(common + 8, v);
    memset(priv, 0, sizeof priv);
    snx_st_le32(priv, sizeof priv);
    snx_st_le32(priv + 4, says);
    priv[0xa] = (u8)mode;
    snx_st_le32(priv + 0xc, styles);
    snx_st_le32(priv + 0x10, hold);
    memset(arg, 0, sizeof arg);
    arg[0] = (u8)(signed char)min;
    arg[1] = (u8)(signed char)max;
    arg[2] = arg[3] = arg[4] = 1; /* the defaults: take over, left justify, two Joy-Cons as one */
}
static void put_text(int slots, int i, const char* text) {
    arg[7 + 4 * slots] = 1;
    snprintf((char*)arg + 7 + 4 * slots + 1 + 0x81 * i, 0x81, "%s", text);
}
static void put_color(int i, u8 r, u8 g, u8 b) {
    arg[6] = 1;
    arg[7 + 4 * i] = r;
    arg[7 + 4 * i + 1] = g;
    arg[7 + 4 * i + 2] = b;
    arg[7 + 4 * i + 3] = 255;
}
static CtrlRequest parsed(u64 size) {
    CtrlRequest r;
    ctrl_request_base(&r, 0, 0);
    assert(!ctrl_request_parse(&r, common, sizeof common, priv, sizeof priv, arg, size));
    return r;
}
static void says(const CtrlRequest* r, const char* head, const char* each) {
    char h[160], e[320];
    ctrl_request_say(r, h, sizeof h, e, sizeof e);
    if (strcmp(h, head) || (each && strcmp(e, each))) {
        fprintf(stderr, "said \"%s\" / \"%s\"\nwant \"%s\" / \"%s\"\n", h, e, head, each ? each : "(any)");
        abort();
    }
}

/* ---- the dialog's quads and pictures */
#define SW 1280
#define SH 720
static SnxUiQuad quads[8192];
static SnxUiList list;
static u32* atlas;
static int atlas_h;
static void draw(int unused) {
    (void)unused;
    list = (SnxUiList){quads, 0, (int)(sizeof quads / sizeof *quads)};
    ctrl_dialog_draw(&list, SW, SH, 2);
    assert(list.n > 0 && list.n < list.max);
}
/* the letters drawn, in the order drawn, spaces left out: whether text (also without spaces) is among them */
static int drew(const char* text) {
    static char all[sizeof quads / sizeof *quads + 1];
    char want[256];
    int n = 0, k = 0;
    for (int i = 0; i < list.n; ++i) {
        SnxUiQuadView v = ui_view(&quads[i]);
        if (v.ink == UI_GLYPH && v.glyph != ' ') all[n++] = (char)v.glyph;
    }
    all[n] = 0;
    for (; *text && k < (int)sizeof want - 1; ++text)
        if (*text != ' ') want[k++] = *text;
    want[k] = 0;
    return strstr(all, want) != NULL;
}
/* whether a quad of the game's color r, g, b is drawn (as a quad keeps it) */
static int drew_color(u8 r, u8 g, u8 b) {
    u32 want = ui_rgba(0xFF000000u | (u32)b << 16 | (u32)g << 8 | r);
    for (int i = 0; i < list.n; ++i)
        if (ui_view(&quads[i]).rgba == want) return 1;
    return 0;
}
static void shot(const char* name, int notice) {
    const char* dir = getenv("SNX_TEST_CTRL_SHOTS");
    static u8 picture[SW * SH * 4];
    static const u8 back[4] = {40, 80, 120, 255};
    char path[1024];
    draw(notice);
    ui_raster(quads, list.n, atlas, atlas_h, picture, SW, SH, back);
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    SDL_Surface* s = SDL_CreateSurfaceFrom(SW, SH, SDL_PIXELFORMAT_RGBA32, picture, SW * 4);
    assert(s && SDL_SavePNG(s, path));
    SDL_DestroySurface(s);
    printf("saved %s\n", path);
}
static CtrlPad host(const char* name) {
    CtrlPad p = {.present = 1, .kind = CTRL_PAD_HOST};
    snprintf(p.name, sizeof p.name, "%s", name);
    return p;
}
/* ---- the dialog's scan, on the fake (no Bluetooth is touched): the host's side recorded */
static struct {
    u8 known_last; /* the last byte of the address the runtime knows */
    int connects;
    u8 last[6];
    int syncing, right;
} host_seen;
static int fake_known(void* user, const u8 address[6]) {
    (void)user;
    return address[5] == host_seen.known_last;
}
static u8 fake_held_last; /* the last byte of the address the host holds off since a Disconnect, 0 none */
static int fake_held_heard;
static int fake_held(void* user, const u8 address[6], int wake) {
    (void)user, (void)wake;
    if (address[5] != fake_held_last) return 0;
    fake_held_heard++;
    return 1;
}
static int fake_busy_now; /* a connect still going on, as the host says */
static int fake_busy(void* user) {
    (void)user;
    return fake_busy_now;
}
static int fake_has_now; /* the host holds the controllers it was asked to connect */
static int fake_has(void* user, const u8 address[6]) {
    (void)user, (void)address;
    return fake_has_now;
}
static void fake_connect(void* user, const u8 address[6], int random, int right, int syncing) {
    (void)user, (void)random;
    host_seen.connects++;
    memcpy(host_seen.last, address, 6);
    host_seen.syncing = syncing;
    host_seen.right = right;
}
/* a Joy-Con 2's advert as the scan hears it: the left or the right one, waiting to sync or paired with its console */
static SnxBleAdvert jc2_advert(u8 last, int right, int syncing) {
    static const u8 data[16] = {0x01, 0x00, 0x03, 0x7e, 0x05, 0x67, 0x20, 0x00, 0x01, 0x80};
    SnxBleAdvert a;
    memset(&a, 0, sizeof a);
    a.address[0] = 0x98;
    a.address[1] = 0xb6;
    a.address[5] = last;
    a.random = 1;
    a.company = SNX_BLE_NINTENDO;
    memcpy(a.data, data, sizeof data);
    a.data[5] = right ? 0x66 : 0x67;
    if (!syncing) memcpy(a.data + 10, "\x11\x22\x33\x44\x55\x66", 6);
    a.size = 16;
    return a;
}
#define B_A (1ull << 0)
#define B_L (1ull << 6)
#define B_R (1ull << 7)
static CtrlPad keyboard(void) {
    CtrlPad p = {.kind = CTRL_PAD_HOST, .keyboard = 1};
    return p;
}

int main(void) {
    CtrlRequest r;
    CtrlPad pads[CTRL_PLAYERS];
    CtrlDo d;
    u32 ticket;
    int first;
    u8 out[CTRL_RESULT_SIZE];
    int checks = 0;

    /* ---- the request: an old SDK's 4 players, single Joy-Cons held sideways, texts and colors */
    build(4, 0, CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT, 1, ARG4, 2, 4);
    put_color(0, 230, 40, 40);
    put_color(1, 40, 90, 230);
    put_text(4, 0, "Red team");
    put_text(4, 1, "Blue team");
    r = parsed(ARG4);
    assert(r.version == 4 && r.slots == 4 && r.min == 2 && r.max == 4 && r.hold == 1 && !r.mouse && !r.single);
    assert(r.has_colors && r.color[1][2] == 230 && r.has_texts && !strcmp(r.text[1], "Blue team") && !r.text[2][0]);
    assert(r.take_over && r.left_justify && r.permit_dual && r.mode == CTRL_MODE_SUPPORT);
    says(&r, "This game needs 2 to 4 players.", "Each player: one Joy-Con held sideways.");
    assert(ctrl_host_kind(&r) == CTRL_PAD_JOY_LEFT && ctrl_request_cards(&r) == 4);
    ++checks;
    /* a newer SDK's 8 players: the private argument says the largest size whatever is pushed, the size decides */
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL | CTRL_STYLE_HANDHELD, 0, ARG8, 0, 4);
    put_text(8, 7, "Eighth");
    r = parsed(ARG8);
    assert(r.slots == 8 && r.min == 0 && r.max == 4 && !strcmp(r.text[7], "Eighth") && !r.has_colors);
    says(&r, "This game needs 1 to 4 players.", "Each player: a Pro Controller or two Joy-Cons together.");
    assert(ctrl_host_kind(&r) == -1);
    ++checks;
    /* single mode: one player whatever the counts */
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 4);
    arg[5] = 1;
    r = parsed(ARG8);
    assert(r.single && r.min == 1 && r.max == 1 && ctrl_request_cards(&r) == 1);
    says(&r, "This game needs 1 player.", "1 player: a Pro Controller or two Joy-Cons together.");
    ++checks;
    /* a most below the least is the least, past the layout's the layout's, a negative least none */
    build(9, 0, CTRL_STYLE_FULL_KEY, 0, ARG8, 3, 2);
    r = parsed(ARG8);
    assert(r.min == 3 && r.max == 3);
    build(4, 0, CTRL_STYLE_FULL_KEY, 0, ARG4, -1, 9);
    r = parsed(ARG4);
    assert(r.min == 0 && r.max == 4);
    ++checks;
    /* the modes: the strap guide with its own small argument, firmware, both remapping numbers, a garbage byte told by
       the size */
    build(9, 1, 0, 0, ARG8, 0, 0);
    ctrl_request_base(&r, CTRL_STYLE_FULL_KEY, 0);
    assert(ctrl_request_parse(&r, common, sizeof common, priv, sizeof priv, arg, 0x20) == -1 &&
           r.mode == CTRL_MODE_STRAP_GUIDE);
    says(&r, "How to put the wrist straps on the Joy-Cons.", NULL);
    build(9, 2, 0, 0, ARG8, 0, 0);
    r = parsed(4);
    assert(r.mode == CTRL_MODE_FIRMWARE);
    build(9, 4, 0, 0, ARG8, 0, 0);
    assert(parsed(0x10).mode == CTRL_MODE_REMAP);
    build(5, 3, 0, 0, 0x10, 0, 0);
    assert(parsed(0x10).mode == CTRL_MODE_REMAP);
    build(5, 0xc8, 0, 0, 4, 0, 0);
    assert(parsed(4).mode == CTRL_MODE_FIRMWARE);
    build(5, 0xc8, CTRL_STYLE_FULL_KEY, 0, ARG4, 1, 2);
    r = parsed(ARG4);
    assert(r.mode == CTRL_MODE_SUPPORT && r.max == 2);
    ++checks;
    /* nothing pushed: the base stays, and so does hid's word for the styles */
    ctrl_request_base(&r, CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2, 1);
    assert(ctrl_request_parse(&r, NULL, 0, NULL, 0, NULL, 0) == -1 && r.mouse && r.min == 1 && r.max == 1);
    says(&r, "This game needs 1 player.", "1 player: two Joy-Con 2 together or one Joy-Con 2 (L) held sideways."); /* no mouse said */
    ++checks;
    /* the result */
    ctrl_result_bytes(out, 2, 1, 0);
    assert(out[0] == 2 && snx_ld_le32(out + 4) == 1 && snx_ld_le32(out + 8) == 0);
    ctrl_result_bytes(out, 1, 0x20, 1);
    assert(out[0] == 1 && snx_ld_le32(out + 4) == 0x20 && snx_ld_le32(out + 8) == 2);
    ++checks;

    assert(SDL_Init(0));
    atlas = ui_atlas_make(&atlas_h);
    assert(atlas && atlas_h > 0);

    /* ---- (a) one player, a Pro Controller or two Joy-Cons: the keyboard is player 1 until a pad takes it */
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL | CTRL_STYLE_HANDHELD, 0, ARG8, 1, 1);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 7);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[2] = host("Xbox Wireless Controller");
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(0) && ctrl_dialog_ready_count(&first) == 1 && first == 0);
    draw(0);
    assert(drew("This game needs 1 player.") && drew("1 player: a Pro Controller or two Joy-Cons together."));
    assert(drew("Ready: the keyboard") && drew("Also here: Xbox Wireless Controller"));
    assert(drew("Join: L on one Joy-Con and R on the other, or L+R on a pad.")); /* no single Joy-Con: no SL+SR */
    /* L and R together on the pad that isn't a player: whole, it takes player 1 from the keyboard */
    d = ctrl_dialog_press(2, B_R, B_L | B_R);
    assert(d.what == CTRL_DO_WHOLE && d.from == 2 && d.card == 0);
    pads[0] = pads[2];
    pads[2] = (CtrlPad){0};
    ctrl_dialog_pads(pads);
    draw(0);
    assert(drew("Ready: Xbox Wireless Controller") &&
           drew("Xbox Wireless Controller is player 1 now, a Pro Controller."));
    shot("ctrl-a-one-player", 0);
    /* A on a player's pad picks the item in focus: Continue */
    d = ctrl_dialog_press(0, 1, 0);
    assert(d.what == CTRL_DO_ANSWERED && !ctrl_dialog_shown() && ctrl_dialog_take(&ticket) == 0 && ticket == 7);
    assert(ctrl_dialog_take(&ticket) == -1);
    ++checks;

    /* ---- (b) 2 to 4 players, one Joy-Con held sideways each, the game's texts and colors */
    build(4, 0, CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT, 1, ARG4, 2, 4);
    put_color(0, 230, 40, 40);
    put_color(1, 40, 90, 230);
    put_color(2, 40, 200, 80);
    put_color(3, 240, 200, 30);
    put_text(4, 0, "Red team");
    put_text(4, 1, "Blue team");
    put_text(4, 2, "Green team");
    put_text(4, 3, "Yellow team");
    r = parsed(ARG4);
    ctrl_dialog_show(&r, 8);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[1] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_LEFT, .name = "Joy-Con (L)"};
    pads[5] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_RIGHT, .name = "Joy-Con (R)"};
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(0) && ctrl_dialog_ready(1) && !ctrl_dialog_ready(2) && !ctrl_dialog_ready(3));
    draw(0);
    assert(drew("P1 Red team") && drew("P4 Yellow team") && drew("Each player: one Joy-Con held sideways."));
    assert(drew("Ready: Joy-Con (L)") && drew("SL+SR on one Joy-Con held sideways, or L+R on a pad."));
    /* a right Joy-Con's R alone is no gesture, nor its SR alone */
    assert(ctrl_dialog_press(5, B_R, B_R).what == CTRL_DO_NONE && ctrl_dialog_press(5, 1ull << 27, 1ull << 27).what ==
                                                                        CTRL_DO_NONE);
    assert(drew("Ready.") && drew("More players can still join."));
    assert(drew_color(230, 40, 40) && drew_color(240, 200, 30));
    /* SL and SR together on the right Joy-Con that waits: alone, sideways, player 3 */
    d = ctrl_dialog_press(5, 1ull << 26, 1ull << 26 | 1ull << 27);
    assert(d.what == CTRL_DO_SINGLE && d.from == 5 && d.card == 2 && d.side == 1);
    pads[2] = pads[5];
    pads[5] = (CtrlPad){0};
    ctrl_dialog_pads(pads);
    shot("ctrl-b-sideways-teams", 0);
    /* X on a player's controller identifies it. Down and Enter is Cancel */
    d = ctrl_dialog_press(1, 1ull << 2, 0);
    assert(d.what == CTRL_DO_IDENTIFY && d.from == 1);
    ctrl_dialog_key(CTRL_KEY_DOWN);
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 1 && ticket == 8);
    ++checks;

    /* ---- (d) a missing player, and a controller that doesn't fit: both Joy-Cons as one where single ones are asked */
    ctrl_dialog_show(&r, 9);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[1] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_PAIR, .name = "Joy-Con (L/R)"};
    pads[5] = host("8BitDo Pro 2");
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(0) && !ctrl_dialog_ready(1));
    /* Continue is refused short of the least, and says what is missing. A on a pad only works the items: no join */
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_NONE && ctrl_dialog_shown());
    d = ctrl_dialog_press(5, B_A, B_A);
    assert(d.what == CTRL_DO_NONE && ctrl_dialog_shown());
    draw(0);
    assert(drew("This game does not accept both Joy-Cons as one player.") && drew("Missing: player 2"));
    assert(drew("SL+SR on one of them: that Joy-Con alone, sideways.") && drew("Continue (players missing)"));
    /* L and R together on the pair: both Joy-Cons as one player, which this game does not take, refused in its card */
    d = ctrl_dialog_press(1, B_L, B_L | B_R);
    assert(d.what == CTRL_DO_NONE);
    draw(0);
    assert(drew("This game does not take both Joy-Cons as one player: press SL and SR on one of them."));
    shot("ctrl-d-missing-misfit", 0);
    /* SL and SR on the pair's left half: it splits, the left half stays player 2, the right one goes where hid puts it
       (player 3 here), each a single the game takes */
    d = ctrl_dialog_press(1, 1ull << 25, 1ull << 24 | 1ull << 25);
    assert(d.what == CTRL_DO_SPLIT && d.from == 1 && d.card == -1 && d.side == 0);
    pads[1].split = 3;
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(1) && ctrl_dialog_ready(2) && !ctrl_dialog_ready(3));
    draw(0);
    assert(drew("Ready: Joy-Con (L/R), left half") && drew("Ready: Joy-Con (L/R), right half") && drew("Ready."));
    pads[1].split = 0;
    ctrl_dialog_pads(pads);
    /* a tap on Cancel */
    {
        int y;
        for (y = SH - 1; y > 0; --y) {
            d = ctrl_dialog_tap(SW / 2.0f, (float)y, SW, SH, 2);
            if (d.what != CTRL_DO_NONE) break;
        }
        assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 1 && ticket == 9);
    }
    ++checks;

    /* ---- (f) L on a left Joy-Con and R on a right one held together: one pair, on the left one's player. A game of
       pairs only takes it, a game of single Joy-Cons refuses it. SL and SR on the left one again: alone */
    build(9, 0, CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 2);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 14);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[4] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_LEFT, .name = "Joy-Con (L)"};
    pads[5] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_RIGHT, .name = "Joy-Con (R)"};
    ctrl_dialog_pads(pads);
    draw(0);
    assert(drew("L on one Joy-Con and R on the other together, or L+R on a pad."));
    d = ctrl_dialog_press(4, B_L, B_L); /* L first, nothing yet */
    assert(d.what == CTRL_DO_NONE);
    d = ctrl_dialog_press(5, B_R, B_R); /* R while L is held */
    assert(d.what == CTRL_DO_MERGE && d.from == 4 && d.to == 5 && d.card == 1);
    pads[1] = pads[4];
    pads[4] = (CtrlPad){0};
    pads[1].merged = 6;
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(1));
    draw(0);
    assert(drew("Ready: Joy-Con (L) + Joy-Con (R)") && !drew("Also here")); /* the right one plays in the pair */
    shot("ctrl-f-pair-gesture", 0);
    d = ctrl_dialog_press(1, 1ull << 24, 1ull << 24 | 1ull << 25);
    assert(d.what == CTRL_DO_NONE); /* this game takes no single Joy-Con: refused */
    draw(0);
    assert(drew("This game does not take one Joy-Con sideways: press L and R on two Joy-Cons together."));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 14);
    /* the right one on a lower player than the left one: the pair plays on the right one's card, nothing moves */
    ctrl_dialog_show(&r, 19);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[1] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_RIGHT, .name = "Joy-Con (R)"};
    pads[3] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_LEFT, .name = "Joy-Con (L)"};
    ctrl_dialog_pads(pads);
    ctrl_dialog_press(1, B_R, B_R);
    d = ctrl_dialog_press(3, B_L, B_L);
    assert(d.what == CTRL_DO_MERGE && d.from == 3 && d.to == 1 && d.card == -1);
    pads[1].merged = 4; /* as hid puts it, on the lower player */
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(1));
    draw(0);
    assert(drew("Ready: Joy-Con (L) + Joy-Con (R)") && drew("are player 2 now") && !drew("Also here") &&
           !drew("Missing"));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 19);
    build(4, 0, CTRL_STYLE_JOY_LEFT | CTRL_STYLE_JOY_RIGHT, 1, ARG4, 1, 2);
    r = parsed(ARG4);
    ctrl_dialog_show(&r, 15);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[4] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_LEFT, .name = "Joy-Con (L)"};
    pads[5] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_RIGHT, .name = "Joy-Con (R)"};
    ctrl_dialog_pads(pads);
    ctrl_dialog_press(5, B_R, B_R);
    d = ctrl_dialog_press(4, B_L, B_L);
    assert(d.what == CTRL_DO_NONE);
    draw(0);
    assert(drew("This game does not take two Joy-Cons as one player: press SL and SR on one of them."));
    d = ctrl_dialog_press(4, 1ull << 25, 1ull << 24 | 1ull << 25);
    assert(d.what == CTRL_DO_SINGLE && d.from == 4 && d.card == 1 && d.side == 0);
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 15);
    ++checks;

    /* ---- (c) the mouse case: only Joy-Con 2 styles, a Joy-Con 2 pair on the table, R held, a new right one found */
    build(9, 0, CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2, 1, ARG8, 1, 1);
    r = parsed(ARG8);
    r.mouse_hands = 2;
    assert(r.mouse);
    ctrl_dialog_show(&r, 10);
    memset(pads, 0, sizeof pads);
    pads[0] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_PAIR, .joycon2 = 1, .name = "Joy-Con 2 (L/R)"};
    ctrl_dialog_pads(pads);
    {
        CtrlMouse m = {{1, 1}, {0, 1}, 60.0f, -30.0f, 1, 0, 18};
        CtrlFound f = {.right = 1, .syncing = 1};
        ctrl_dialog_mouse(&m);
        ctrl_dialog_found(&f, 1);
    }
    assert(ctrl_dialog_ready(0));
    draw(0);
    /* the game never says it uses the mouse: the check is an optional panel, worded "if", and no hand is named */
    assert(!drew("This game uses the Joy-Con 2 as a mouse") && !drew("hand:") && drew("This game needs 1 player."));
    assert(drew("If the game uses the Joy-Con 2 as a mouse: on its side on the table"));
    assert(drew("On the table") && drew("sensor down") && drew("If it uses a mouse, the PC mouse as the right one: Off"));
    assert(drew("It reports only 18 times a second") && drew("Connect the new Joy-Con 2 (R)"));
    shot("ctrl-c-mouse", 0);
    /* down to the PC mouse, picked: on */
    ctrl_dialog_key(CTRL_KEY_DOWN);
    ctrl_dialog_key(CTRL_KEY_DOWN);
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_PC_MOUSE && d.from == 1);
    ctrl_dialog_key(CTRL_KEY_DOWN); /* the Joy-Con 2 pair's Disconnect, then the new one's Connect */
    ctrl_dialog_key(CTRL_KEY_DOWN);
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_TAKE && d.from == 0);
    d = ctrl_dialog_key(CTRL_KEY_ESC);
    assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 1);
    ++checks;

    /* ---- the modes that only explain: a press closes them, as done */
    build(9, 1, 0, 0, ARG8, 0, 0);
    ctrl_request_base(&r, CTRL_STYLE_FULL_KEY, 0);
    ctrl_request_parse(&r, common, sizeof common, priv, sizeof priv, arg, 0x20);
    ctrl_dialog_show(&r, 11);
    draw(0);
    assert(drew("How to put the wrist straps on the Joy-Cons.") && drew("Close"));
    d = ctrl_dialog_press(3, 1, 0);
    assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 0 && ticket == 11);
    ++checks;


    /* ---- the scan while the dialog shows: a known Joy-Con 2 connected as soon as it is heard, a new one waiting to
       sync shown and connected only once confirmed, something else ignored, the scan closed with the dialog. A system
       whose scan doesn't start says why */
    {
        CtrlScanHost h = {fake_known, fake_connect, fake_busy, NULL, NULL, NULL};
        CtrlScan sc;
        CtrlFound found[CTRL_FOUND];
        SnxBleScan* fake = snx_ble_fake_open("");
        SnxBleAdvert left = jc2_advert(1, 0, 0), right = jc2_advert(2, 1, 1), other = jc2_advert(3, 0, 0);
        int n;
        other.company = 0x004c;
        host_seen.known_last = 1;
        assert(fake && !ctrl_scan_open(&sc, fake, NULL) && snx_ble_fake_starts(fake) == 1 && snx_ble_scan_listening(fake));
        assert(!snx_ble_fake_hear(fake, &left) && !snx_ble_fake_hear(fake, &right) && !snx_ble_fake_hear(fake, &other));
        n = ctrl_scan_poll(&sc, &h, found);
        assert(n == 2 && found[0].known && !found[0].right && !found[0].syncing && !found[1].known && found[1].right &&
               found[1].syncing && !found[1].asked);
        assert(host_seen.connects == 1 && host_seen.last[5] == 1 && !host_seen.syncing);
        /* listening stopped for the connect, and again once the host has none going on */
        assert(!snx_ble_scan_listening(fake) && sc.stopped);
        fake_busy_now = 1;
        assert(ctrl_scan_poll(&sc, &h, found) == 2 && !snx_ble_scan_listening(fake));
        fake_busy_now = 0;
        assert(ctrl_scan_poll(&sc, &h, found) == 2 && snx_ble_scan_listening(fake) && snx_ble_fake_starts(fake) == 2);
        /* heard again: no second connect */
        assert(!snx_ble_fake_hear(fake, &left));
        assert(ctrl_scan_poll(&sc, &h, found) == 2 && host_seen.connects == 1 && snx_ble_scan_listening(fake));
        /* in the dialog: the new one's Connect item, which a pick takes */
        build(9, 0, CTRL_STYLE_JOY_DUAL | CTRL_STYLE_FULL_KEY, 0, ARG8, 1, 2);
        r = parsed(ARG8);
        ctrl_dialog_show(&r, 13);
        memset(pads, 0, sizeof pads);
        pads[0] = keyboard();
        ctrl_dialog_pads(pads);
        ctrl_dialog_found(found, n);
        draw(0);
        assert(drew("Connect the new Joy-Con 2 (R)") && drew("Joy-Con 2 (L) found: it connects on its own"));
        ctrl_dialog_key(CTRL_KEY_DOWN);
        ctrl_dialog_key(CTRL_KEY_DOWN);
        d = ctrl_dialog_key(CTRL_KEY_ENTER);
        assert(d.what == CTRL_DO_TAKE && d.from == 1);
        ctrl_scan_take(&sc, &h, d.from);
        assert(host_seen.connects == 2 && host_seen.last[5] == 2 && host_seen.syncing && host_seen.right);
        ctrl_scan_take(&sc, &h, d.from);
        assert(host_seen.connects == 2);
        d = ctrl_dialog_key(CTRL_KEY_ESC);
        assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 1 && ticket == 13);
        ctrl_scan_close(&sc);
        assert(!sc.scan && !sc.n);
        /* a scan that won't start, and none at all */
        assert(ctrl_scan_open(&sc, snx_ble_fake_open("Bluetooth is off"), NULL) == -1 &&
               !strcmp(sc.why, "Bluetooth is off") && !sc.scan);
        assert(ctrl_scan_open(&sc, NULL, "no BlueZ") == -1 && !strcmp(sc.why, "no BlueZ"));
        ++checks;
    }
    /* ---- a known Joy-Con 2 whose connect fails: asked again each time it is heard again, CTRL_SCAN_TRIES times, then
       the scan stops and says the host tries it in turn */
    {
        CtrlScanHost h = {fake_known, fake_connect, fake_busy, NULL, fake_has, NULL};
        CtrlScan sc;
        CtrlFound found[CTRL_FOUND];
        SnxBleScan* fake = snx_ble_fake_open("");
        SnxBleAdvert left = jc2_advert(4, 0, 0);
        int start = host_seen.connects, t;
        host_seen.known_last = 4;
        fake_has_now = 0;
        fake_busy_now = 0;
        assert(fake && !ctrl_scan_open(&sc, fake, NULL));
        for (t = 1; t <= CTRL_SCAN_TRIES; t++) {
            assert(!snx_ble_fake_hear(fake, &left));
            assert(ctrl_scan_poll(&sc, &h, found) == 1 && host_seen.connects == start + t);
            assert(!snx_ble_scan_listening(fake) && sc.stopped);
            ctrl_scan_poll(&sc, &h, found); /* the connect failed: listening again, or given back after the last */
        }
        assert(!sc.scan && sc.gave_up && strstr(sc.why, "tried in turn"));
        assert(ctrl_scan_poll(&sc, &h, found) == 1 && host_seen.connects == start + CTRL_SCAN_TRIES);
        ctrl_scan_close(&sc);
        /* one that connects: heard again, it isn't asked again */
        fake = snx_ble_fake_open("");
        assert(fake && !ctrl_scan_open(&sc, fake, NULL));
        assert(!snx_ble_fake_hear(fake, &left) && ctrl_scan_poll(&sc, &h, found) == 1);
        fake_has_now = 1;
        ctrl_scan_poll(&sc, &h, found);
        assert(snx_ble_scan_listening(fake) && !snx_ble_fake_hear(fake, &left) && ctrl_scan_poll(&sc, &h, found) == 1);
        assert(host_seen.connects == start + CTRL_SCAN_TRIES + 1 && !sc.gave_up);
        ctrl_scan_close(&sc);
        ++checks;
    }

    /* ---- (e) the pause for a controller that left: the dialog says who, A on another pad takes its place, and the
       only item goes on without it */
    ctrl_request_base(&r, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL, 0);
    r.slots = 8;
    r.min = r.max = 2;
    snprintf(r.lost, sizeof r.lost, "P1");
    ctrl_dialog_show(&r, 0);
    memset(pads, 0, sizeof pads);
    pads[0] = keyboard();
    pads[0].lost = 1;
    pads[1] = host("Xbox Wireless Controller");
    pads[2] = host("8BitDo Pro 2");
    ctrl_dialog_pads(pads);
    assert(!ctrl_dialog_ready(0) && ctrl_dialog_ready(1));
    draw(0);
    assert(drew("P1 disconnected.") && drew("Go on without P1") && !drew("Cancel") && drew("Its controller left"));
    d = ctrl_dialog_key(CTRL_KEY_ESC);
    assert(d.what == CTRL_DO_NONE && ctrl_dialog_shown());
    shot("ctrl-e-reconnect", 0);
    d = ctrl_dialog_press(2, B_L, B_L | B_R);
    assert(d.what == CTRL_DO_WHOLE && d.from == 2 && d.card == 0);
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_ANSWERED && ctrl_dialog_take(&ticket) == 0 && ticket == 0);
    ++checks;

    /* ---- (g) attached or detached: player 1 as the Joy-Cons attached to the console (the TV mode off). A game that
       doesn't take the handheld controls refuses it until Detach (a game of Joy-Con 2 alone too), one that does says
       which it is */
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 2);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 16);
    memset(pads, 0, sizeof pads);
    pads[0] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_PAIR, .name = "Joy-Con (L/R)"};
    ctrl_dialog_pads(pads);
    ctrl_dialog_attached(1, 1);
    assert(!ctrl_dialog_ready(0));
    draw(0);
    assert(drew("x Joy-Con (L/R), attached to the console") &&
           drew("This game does not take the Joy-Cons attached to the console: pick Detach below.") &&
           drew("Detach the Joy-Cons from the console (tabletop)") && drew("Missing: player 1"));
    shot("ctrl-g-attached", 0);
    ctrl_dialog_key(CTRL_KEY_DOWN);
    ctrl_dialog_key(CTRL_KEY_DOWN);
    d = ctrl_dialog_key(CTRL_KEY_ENTER);
    assert(d.what == CTRL_DO_DETACH && d.from == 1);
    ctrl_dialog_attached(1, 0);
    assert(ctrl_dialog_ready(0));
    draw(0);
    assert(drew("Detached from the console.") && drew("Attach the Joy-Cons to the console"));
    d = ctrl_dialog_key(CTRL_KEY_ENTER); /* the focus stays on the item: attached again */
    assert(d.what == CTRL_DO_DETACH && d.from == 0);
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 16);
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_HANDHELD | CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 2);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 17);
    ctrl_dialog_attached(1, 1);
    assert(ctrl_dialog_ready(0));
    draw(0);
    assert(drew("Attached to the console."));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1);
    build(9, 0, CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2, 1, ARG8, 1, 1);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 18);
    assert(!ctrl_dialog_ready(0));
    draw(0);
    assert(drew("This game does not take the Joy-Cons attached to the console: pick Detach below.") &&
           !drew("The mouse needs"));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1);
    ctrl_dialog_attached(0, 0); /* docked: no handheld, no item */
    ++checks;

    /* ---- Disconnect: a player's controller the runtime connected itself has its item, which asks the backend to close
       its link. Held off afterwards, the scan hears it without connecting it, and its Connect item takes it back */
    {
        CtrlScanHost h = {fake_known, fake_connect, fake_busy, NULL, NULL, fake_held};
        CtrlScan sc;
        CtrlFound found[CTRL_FOUND];
        SnxBleScan* fake = snx_ble_fake_open("");
        SnxBleAdvert left = jc2_advert(7, 0, 0);
        int start = host_seen.connects, i, n;
        build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 2);
        r = parsed(ARG8);
        ctrl_dialog_show(&r, 20);
        memset(pads, 0, sizeof pads);
        pads[0] = keyboard();
        pads[1] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_PAIR, .joycon2 = 1, .name = "Joy-Con 2 (L/R)"};
        ctrl_dialog_pads(pads);
        draw(0);
        assert(drew("Disconnect P2: Joy-Con 2 (L/R)"));
        for (i = 0; i < 2; i++) ctrl_dialog_key(CTRL_KEY_DOWN); /* Continue, Cancel, Disconnect */
        d = ctrl_dialog_key(CTRL_KEY_ENTER);
        assert(d.what == CTRL_DO_DISCONNECT && d.from == 1);
        pads[1] = (CtrlPad){0}; /* gone: player 2 waits */
        ctrl_dialog_pads(pads);
        assert(!ctrl_dialog_ready(1));
        host_seen.known_last = 7;
        fake_held_last = 7;
        fake_held_heard = 0;
        assert(fake && !ctrl_scan_open(&sc, fake, NULL));
        assert(!snx_ble_fake_hear(fake, &left));
        n = ctrl_scan_poll(&sc, &h, found);
        assert(n == 1 && found[0].known && found[0].held && fake_held_heard == 1 && host_seen.connects == start);
        assert(snx_ble_scan_listening(fake)); /* not stopped for it, nor given up on */
        ctrl_dialog_found(found, n);
        draw(0);
        assert(drew("Connect Joy-Con 2 (L) again") && drew("disconnected, pick Connect below to take it back."));
        for (i = 0; i < 2; i++) ctrl_dialog_key(CTRL_KEY_DOWN); /* Continue, Cancel, Connect again */
        d = ctrl_dialog_key(CTRL_KEY_ENTER);
        assert(d.what == CTRL_DO_TAKE && d.from == 0);
        fake_held_last = 0; /* the host's connect ends the hold */
        ctrl_scan_take(&sc, &h, d.from);
        assert(host_seen.connects == start + 1 && host_seen.last[5] == 7 && !sc.heard[0].found.held);
        ctrl_scan_close(&sc);
        /* a Pro Controller 2 waiting to sync, by its own name */
        {
            CtrlFound pro = {.syncing = 1, .pro = 1};
            ctrl_dialog_found(&pro, 1);
            draw(0);
            assert(drew("Connect the new Pro Controller 2") && drew("Pro Controller 2 found: pick Connect below"));
        }
        ctrl_dialog_key(CTRL_KEY_ESC);
        assert(ctrl_dialog_take(&ticket) == 1 && ticket == 20);
        ++checks;
    }

    /* ---- (h) a console Pro Controller keeps its own style: in a game of Joy-Con 2 alone it doesn't fit (the game saw no
       controller it takes, which is why it asked), its L+R is refused with the reason, and nothing gives it the
       style the game takes. A generic pad there still plays as a Joy-Con 2 pair. In a game of Pro Controllers it is
       ready, and its L+R joins it whole */
    build(9, 0, CTRL_STYLE_JOY_DUAL2 | CTRL_STYLE_JOY_LEFT2, 0, ARG8, 1, 1);
    r = parsed(ARG8);
    assert(r.mouse && ctrl_host_kind(&r) == -1);
    ctrl_dialog_show(&r, 21);
    memset(pads, 0, sizeof pads);
    pads[0] = (CtrlPad){.present = 1, .kind = CTRL_PAD_PRO, .joycon2 = 1, .name = "Pro Controller 2"};
    pads[2] = host("Xbox Wireless Controller");
    ctrl_dialog_pads(pads);
    assert(!ctrl_dialog_ready(0));
    draw(0);
    assert(drew("x Pro Controller 2 does not fit") &&
           drew("This game takes only two Joy-Con 2 together: the Pro Controller does not fit.") &&
           drew("Missing: player 1"));
    shot("ctrl-h-pro-refused", 0);
    d = ctrl_dialog_press(0, B_R, B_L | B_R);
    draw(0);
    assert(d.what == CTRL_DO_NONE && drew("Pro Controller 2: this game takes only"));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 21);
    build(9, 0, CTRL_STYLE_FULL_KEY | CTRL_STYLE_JOY_DUAL, 0, ARG8, 1, 2);
    r = parsed(ARG8);
    ctrl_dialog_show(&r, 22);
    pads[0] = keyboard();
    pads[3] = (CtrlPad){.present = 1, .kind = CTRL_PAD_PRO, .joycon2 = 1, .name = "Pro Controller 2"};
    ctrl_dialog_pads(pads);
    d = ctrl_dialog_press(3, B_R, B_L | B_R);
    assert(d.what == CTRL_DO_WHOLE && d.from == 3 && d.card == 1);
    pads[1] = pads[3];
    pads[3] = (CtrlPad){0};
    ctrl_dialog_pads(pads);
    assert(ctrl_dialog_ready(1));
    draw(0);
    assert(drew("Ready: Pro Controller 2") && drew("is player 2 now, a Pro Controller."));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 22);
    ++checks;

    /* ---- (i) a game of Joy-Con 2 pairs alone with its mouse sensor started (as one game does at setup in every scene,
       a shaking one too) and a sideways hold left as the SDK's default: nothing says mouse or sideways, the pair is
       drawn upright, the mouse check is the optional panel. With no sensor started, no panel at all */
    build(9, 0, CTRL_STYLE_JOY_DUAL2, 1, ARG8, 1, 1);
    r = parsed(ARG8);
    r.mouse_hands = 3;
    assert(r.mouse && r.hold == 1);
    says(&r, "This game needs 1 player.", "1 player: two Joy-Con 2 together.");
    ctrl_dialog_show(&r, 23);
    memset(pads, 0, sizeof pads);
    pads[0] = (CtrlPad){.present = 1, .kind = CTRL_PAD_JOY_PAIR, .joycon2 = 1, .name = "Joy-Con 2 (L/R)"};
    ctrl_dialog_pads(pads);
    {
        CtrlMouse m = {{1, 1}, {0, 0}, 0.0f, 0.0f, 0, 0, 0};
        ctrl_dialog_mouse(&m);
    }
    draw(0);
    assert(drew("1 player: two Joy-Con 2 together.") && !drew("sideways") && !drew("SL") && !drew("This game uses"));
    assert(drew("If the game uses the Joy-Con 2 as a mouse") && drew("In the air: put it down"));
    assert(drew("Ready: Joy-Con 2 (L/R)") && drew("Ready."));
    shot("ctrl-i-pair-maybe-mouse", 0);
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 23);
    r.mouse_hands = 0;
    ctrl_dialog_show(&r, 24);
    draw(0);
    assert(!drew("If the game uses the Joy-Con 2 as a mouse") && !drew("If it uses a mouse"));
    ctrl_dialog_key(CTRL_KEY_ESC);
    assert(ctrl_dialog_take(&ticket) == 1 && ticket == 24);
    ++checks;

    /* a hide for another ticket leaves the dialog, its own takes it away unanswered */
    ctrl_dialog_show(&r, 12);
    ctrl_dialog_hide(11);
    assert(ctrl_dialog_shown());
    ctrl_dialog_hide(12);
    assert(!ctrl_dialog_shown() && ctrl_dialog_take(&ticket) == -1);
    ++checks;

    free(atlas);
    SDL_Quit();
    printf("PASS ctrl_applet: %d checks\n", checks);
    return 0;
}
