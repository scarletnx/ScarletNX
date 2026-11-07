/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A skin for the on-screen controls (vpad.c): a RetroArch overlay, its .cfg and the PNGs it names, as libretro's
   common-overlays and RetroArch's own overlays are made. Its first page not named portrait is read: each descriptor
   whose name is one of the controls gives that control its default place, its size, its hit area (radial or rect,
   range_mod, reach) and its images, which go into rows of the atlas under its picture (ui.h). A two-frame APNG is
   an image and its pressed look, as RetroArch draws one; a one-frame image is drawn pressed at its alpha times
   alpha_mod. Every descriptor left out is logged with why, and the controls the skin has none for keep the built-in
   look. Loaded before the window opens, as the atlas is made then */
#ifndef SNX_VPAD_SKIN_H
#define SNX_VPAD_SKIN_H
#include "common/cpu/aarch64/cpu.h"

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

/* the overlay at path: 1 read, its controls in place (vpad_skin_set) and its images in the atlas (ui_skin_set); 0
   when it can't be, said in the log, and the built-in look */
int vpad_skin_load(const char* path);
#endif
