/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad_consoles.h"

/* Each is the Switch's control where it sits, whose bit the console's window takes a gamepad's by too: Start is Plus
   and Select Minus */
static const SnxVpadControl gba[] = {
    {SNX_VPAD_L, "L", "L", {0.10f, 0.12f}, {0.16f, 0.68f}},
    {SNX_VPAD_R, "R", "R", {0.90f, 0.12f}, {0.84f, 0.68f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.07f, 0.40f}, {0.40f, 0.94f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.93f, 0.40f}, {0.60f, 0.94f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.10f, 0.72f}, {0.20f, 0.82f}},
    {SNX_VPAD_A, "A", "A", {0.94f, 0.65f}, {0.88f, 0.79f}},
    {SNX_VPAD_B, "B", "B", {0.87f, 0.79f}, {0.72f, 0.85f}},
};
const SnxVpadConsole snx_vpad_gba = {"GBA", gba, sizeof gba / sizeof *gba, 0};

static const SnxVpadControl nds[] = {
    {SNX_VPAD_L, "L", "L", {0.09f, 0.12f}, {0.16f, 0.077f}},
    {SNX_VPAD_R, "R", "R", {0.91f, 0.12f}, {0.84f, 0.077f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.07f, 0.40f}, {0.41f, 0.965f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.93f, 0.36f}, {0.53f, 0.965f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.11f, 0.70f}, {0.17f, 0.923f}},
    {SNX_VPAD_A, "A", "A", {0.94f, 0.66f}, {0.90f, 0.923f}},
    {SNX_VPAD_B, "B", "B", {0.885f, 0.79f}, {0.80f, 0.968f}},
    {SNX_VPAD_X, "X", "X", {0.885f, 0.53f}, {0.80f, 0.878f}},
    {SNX_VPAD_Y, "Y", "Y", {0.83f, 0.66f}, {0.70f, 0.923f}},
};
const SnxVpadConsole snx_vpad_nds = {"DS", nds, sizeof nds / sizeof *nds, 1};

static const SnxVpadControl vita[] = {
    {SNX_VPAD_L, "L", "L", {0.09f, 0.10f}, {0.16f, 0.67f}},
    {SNX_VPAD_R, "R", "R", {0.91f, 0.10f}, {0.84f, 0.67f}},
    {SNX_VPAD_MINUS, "Select", "SE", {0.05f, 0.92f}, {0.42f, 0.96f}},
    {SNX_VPAD_PLUS, "Start", "ST", {0.95f, 0.92f}, {0.58f, 0.96f}},
    {SNX_VPAD_DPAD, "D-pad", "", {0.10f, 0.42f}, {0.17f, 0.77f}},
    {SNX_VPAD_STICK_L, "Left stick", "", {0.20f, 0.78f}, {0.20f, 0.91f}},
    {SNX_VPAD_STICK_R, "Right stick", "", {0.78f, 0.78f}, {0.80f, 0.91f}},
    {SNX_VPAD_A, "Circle", "O", {0.955f, 0.42f}, {0.91f, 0.77f}},
    {SNX_VPAD_B, "Cross", "X", {0.90f, 0.55f}, {0.80f, 0.815f}},
    {SNX_VPAD_X, "Triangle", "\x1e", {0.90f, 0.29f}, {0.80f, 0.725f}},
    {SNX_VPAD_Y, "Square", "\xfe", {0.845f, 0.42f}, {0.69f, 0.77f}},
};
const SnxVpadConsole snx_vpad_vita = {"Vita", vita, sizeof vita / sizeof *vita, 1};
