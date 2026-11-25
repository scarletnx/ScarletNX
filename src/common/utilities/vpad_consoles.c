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
