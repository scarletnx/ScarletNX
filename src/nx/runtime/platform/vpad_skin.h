/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A skin for the on-screen controls (vpad.c): a RetroArch overlay, its .cfg and the PNGs it names, as libretro's
   common-overlays and RetroArch's own overlays are made. Of its pages not named portrait, the one that gives the most
   controls a look is read (the first of those): each descriptor whose name is one of the controls gives that control
   its default place, its size, its hit area (radial or rect, range_mod, reach) and its images, which go into rows of
   the atlas under its picture (ui.h). A two-frame APNG is
   an image and its pressed look, as RetroArch draws one; a one-frame image is drawn pressed at its alpha times
   alpha_mod. Every descriptor left out is logged with why, and the controls the skin has none for keep the built-in
   look. controls_skin (settings.txt) names the skin: a shipped one (dist/controls/skins, compiled in) or a .cfg's
   path; the backend applies it when the window opens and again whenever the setting changes (vpad_skin_changed) */
#ifndef SNX_VPAD_SKIN_H
#define SNX_VPAD_SKIN_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

#define SNX_SKIN_PIECES   4
/* a piece's lit: its control held */
#define SNX_SKIN_LIT_HELD 0x10000u
/* one image drawn on a control */
typedef struct {
    u16 u, v, uw, vh; /* its texels in the atlas */
    u16 pu, pv;       /* its pressed look's, as large: u and v again for a one-frame image */
    float x, y, w, h; /* where on the control's box: 0 to 1 across and down */
    u32 lit;          /* when it is drawn pressed: SNX_SKIN_LIT_HELD, or the D-pad's direction bits (vpad.c) */
} SnxSkinPiece;
/* a control's look and hit area from the skin */
typedef struct {
    float x, y;      /* its default middle, 0 to 1 across the clear part */
    float w, h;      /* its box, of the clear part's shorter side */
    float reach[4];  /* its hit area from the middle, of half the box: left, right, up, down (range_mod and reach) */
    float alpha_mod; /* a one-frame piece pressed: its alpha times this, up to opaque */
    float saturate;  /* a stick: pushed all the way at this share of half its box */
    int round;       /* the hit area an ellipse, else a rectangle */
    int knob;        /* a stick's piece that moves with the finger, -1 for none */
    int pieces;      /* 0: the built-in look */
    SnxSkinPiece piece[SNX_SKIN_PIECES];
} SnxSkinControl;

/* the skin a controls_skin value names: empty for the built-in look, a shipped skin's name, else a .cfg's path. Its
   controls into at (pieces 0 for the built-in look) and its images to ui_skin_set for the next atlas; 1, or 0 when it
   can't be read, said in the log, and at and the atlas's rows the built-in look's. On the main thread, not under
   ui_lock */
int vpad_skin_read(const char* value, SnxSkinControl* at);
/* controls_skin's skin, read, then the atlas made from sprites (the built-in image, w x h, as ui_atlas_set takes it;
   NULL for none) with its images, and its controls put in at once, under ui_lock: what vpad_skin_read returned. On the
   main thread, not under ui_lock */
int vpad_skin_apply(const u32* sprites, int w, int h);
#endif
