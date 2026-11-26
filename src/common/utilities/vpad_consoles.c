/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad_consoles.h"

/* Each is the Switch's control where it sits, whose bit the console's window takes a gamepad's by too: Start is Plus
   and Select Minus. Wide, the Switch's place but where one is given: the GBA's and the DS's L right over the D-pad, the
   Vita's Select and Start beside L and R. Upright, under the picture: L and R over the D-pad and the buttons, the Vita's
   Select and Start beside them and its sticks where the Switch's are */
static const SnxVpadControl gba[] = {
    {SNX_VPAD_L, "L", "L", {0.230f, 0.580f}, {0.160f, 0.680f}},
    {SNX_VPAD_R, "R", "R", {0.000f, 0.000f}, {0.840f, 0.680f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.000f, 0.000f}, {0.400f, 0.940f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.000f, 0.000f}, {0.600f, 0.940f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.000f, 0.000f}, {0.200f, 0.820f}},
    {SNX_VPAD_A, "A", "A", {0.000f, 0.000f}, {0.880f, 0.790f}},
    {SNX_VPAD_B, "B", "B", {0.000f, 0.000f}, {0.720f, 0.850f}},
};
const SnxVpadConsole snx_vpad_gba = {"GBA", gba, sizeof gba / sizeof *gba, 0};

static const SnxVpadControl nds[] = {
    {SNX_VPAD_L, "L", "L", {0.230f, 0.580f}, {0.170f, 0.820f}},
    {SNX_VPAD_R, "R", "R", {0.000f, 0.000f}, {0.800f, 0.820f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.000f, 0.000f}, {0.410f, 0.965f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.000f, 0.000f}, {0.530f, 0.965f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.000f, 0.000f}, {0.170f, 0.923f}},
    {SNX_VPAD_A, "A", "A", {0.000f, 0.000f}, {0.900f, 0.923f}},
    {SNX_VPAD_B, "B", "B", {0.000f, 0.000f}, {0.800f, 0.968f}},
    {SNX_VPAD_X, "X", "X", {0.000f, 0.000f}, {0.800f, 0.878f}},
    {SNX_VPAD_Y, "Y", "Y", {0.000f, 0.000f}, {0.700f, 0.923f}},
};
const SnxVpadConsole snx_vpad_nds = {"DS", nds, sizeof nds / sizeof *nds, 1};

static const SnxVpadControl vita[] = {
    {SNX_VPAD_L, "L", "L", {0.000f, 0.000f}, {0.160f, 0.670f}},
    {SNX_VPAD_R, "R", "R", {0.000f, 0.000f}, {0.840f, 0.670f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.210f, 0.210f}, {0.360f, 0.670f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.790f, 0.210f}, {0.640f, 0.670f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.000f, 0.000f}, {0.270f, 0.935f}},
    {SNX_VPAD_STICK_L, "Left stick", "", {0.000f, 0.000f}, {0.120f, 0.809f}},
    {SNX_VPAD_STICK_R, "Right stick", "", {0.000f, 0.000f}, {0.770f, 0.921f}},
    {SNX_VPAD_A, "Circle", "O", {0.000f, 0.000f}, {0.910f, 0.770f}},
    {SNX_VPAD_B, "Cross", "X", {0.000f, 0.000f}, {0.800f, 0.815f}},
    {SNX_VPAD_X, "Triangle", "\x1e", {0.000f, 0.000f}, {0.800f, 0.725f}},
    {SNX_VPAD_Y, "Square", "\xfe", {0.000f, 0.000f}, {0.690f, 0.770f}},
};
const SnxVpadConsole snx_vpad_vita = {"Vita", vita, sizeof vita / sizeof *vita, 1};

/* the 3DS's, both screens over them upright (to 55% of the height): wide, Select and Start beside L and R and Home over
   Start; upright, the Vita's arrangement under the screens */
static const SnxVpadControl ctr[] = {
    {SNX_VPAD_L, "L", "L", {0.000f, 0.000f}, {0.160f, 0.630f}},
    {SNX_VPAD_R, "R", "R", {0.000f, 0.000f}, {0.840f, 0.630f}},
    {SNX_VPAD_ZL, "ZL", "ZL", {0.000f, 0.000f}, {0.160f, 0.585f}},
    {SNX_VPAD_ZR, "ZR", "ZR", {0.000f, 0.000f}, {0.840f, 0.585f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.210f, 0.210f}, {0.360f, 0.630f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.790f, 0.210f}, {0.640f, 0.630f}},
    {SNX_VPAD_HOME, "Home", "\x7f", {0.790f, 0.080f}, {0.500f, 0.965f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.000f, 0.000f}, {0.270f, 0.925f}},
    {SNX_VPAD_STICK_L, "Circle Pad", "", {0.000f, 0.000f}, {0.120f, 0.770f}},
    {SNX_VPAD_STICK_R, "C-Stick", "", {0.000f, 0.000f}, {0.770f, 0.915f}},
    {SNX_VPAD_A, "A", "A", {0.000f, 0.000f}, {0.910f, 0.740f}},
    {SNX_VPAD_B, "B", "B", {0.000f, 0.000f}, {0.800f, 0.785f}},
    {SNX_VPAD_X, "X", "X", {0.000f, 0.000f}, {0.800f, 0.695f}},
    {SNX_VPAD_Y, "Y", "Y", {0.000f, 0.000f}, {0.690f, 0.740f}},
};
const SnxVpadConsole snx_vpad_3ds = {"3DS", ctr, sizeof ctr / sizeof *ctr, 1};
