// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

/** Gravity constants and the placement of a box in a container. */
public class Gravity {
    public static final int NO_GRAVITY = 0, AXIS_SPECIFIED = 1, AXIS_PULL_BEFORE = 2, AXIS_PULL_AFTER = 4, AXIS_CLIP = 8,
        AXIS_X_SHIFT = 0, AXIS_Y_SHIFT = 4, TOP = 0x30, BOTTOM = 0x50, LEFT = 0x03, RIGHT = 0x05, CENTER_VERTICAL = 0x10,
        FILL_VERTICAL = 0x70, CENTER_HORIZONTAL = 0x01, FILL_HORIZONTAL = 0x07, CENTER = 0x11, FILL = 0x77,
        CLIP_VERTICAL = 0x80, CLIP_HORIZONTAL = 0x08, RELATIVE_LAYOUT_DIRECTION = 0x00800000, HORIZONTAL_GRAVITY_MASK = 0x07,
        VERTICAL_GRAVITY_MASK = 0x70, DISPLAY_CLIP_VERTICAL = 0x10000000, DISPLAY_CLIP_HORIZONTAL = 0x01000000,
        START = RELATIVE_LAYOUT_DIRECTION | LEFT, END = RELATIVE_LAYOUT_DIRECTION | RIGHT,
        RELATIVE_HORIZONTAL_GRAVITY_MASK = START | END;

    public static int getAbsoluteGravity(int gravity, int layoutDirection) {
        if ((gravity & RELATIVE_LAYOUT_DIRECTION) == 0) return gravity;
        int g = gravity & ~RELATIVE_LAYOUT_DIRECTION;
        boolean rtl = layoutDirection == View.LAYOUT_DIRECTION_RTL;
        if ((g & LEFT) == LEFT && (g & RIGHT) != RIGHT) g = (g & ~LEFT) | (rtl ? RIGHT : LEFT);
        else if ((g & RIGHT) == RIGHT) g = (g & ~RIGHT) | (rtl ? LEFT : RIGHT);
        return g;
    }

    public static void apply(int gravity, int w, int h, android.graphics.Rect container, android.graphics.Rect outRect) {
        apply(gravity, w, h, container, 0, 0, outRect);
    }

    public static void apply(int gravity, int w, int h, android.graphics.Rect container, int xAdj, int yAdj, android.graphics.Rect outRect) {
        switch (gravity & HORIZONTAL_GRAVITY_MASK) {
            case CENTER_HORIZONTAL:
                outRect.left = container.left + (container.width() - w) / 2 + xAdj;
                outRect.right = outRect.left + w;
                break;
            case RIGHT:
                outRect.right = container.right - xAdj;
                outRect.left = outRect.right - w;
                break;
            case FILL_HORIZONTAL:
                outRect.left = container.left;
                outRect.right = container.right;
                break;
            default:
                outRect.left = container.left + xAdj;
                outRect.right = outRect.left + w;
                break;
        }
        switch (gravity & VERTICAL_GRAVITY_MASK) {
            case CENTER_VERTICAL:
                outRect.top = container.top + (container.height() - h) / 2 + yAdj;
                outRect.bottom = outRect.top + h;
                break;
            case BOTTOM:
                outRect.bottom = container.bottom - yAdj;
                outRect.top = outRect.bottom - h;
                break;
            case FILL_VERTICAL:
                outRect.top = container.top;
                outRect.bottom = container.bottom;
                break;
            default:
                outRect.top = container.top + yAdj;
                outRect.bottom = outRect.top + h;
                break;
        }
    }

    public static void apply(int gravity, int w, int h, android.graphics.Rect container, android.graphics.Rect outRect, int layoutDirection) {
        apply(getAbsoluteGravity(gravity, layoutDirection), w, h, container, outRect);
    }

    public static void apply(int gravity, int w, int h, android.graphics.Rect container, int xAdj, int yAdj, android.graphics.Rect outRect, int layoutDirection) {
        apply(getAbsoluteGravity(gravity, layoutDirection), w, h, container, xAdj, yAdj, outRect);
    }

    /** Moves an object into the display, or clips it to the display with the DISPLAY_CLIP flags. */
    public static void applyDisplay(int gravity, android.graphics.Rect display, android.graphics.Rect inoutObj) {
        if ((gravity & DISPLAY_CLIP_VERTICAL) != 0) {
            inoutObj.top = Math.max(inoutObj.top, display.top);
            inoutObj.bottom = Math.min(inoutObj.bottom, display.bottom);
        } else {
            int dy = inoutObj.top < display.top ? display.top - inoutObj.top
                : inoutObj.bottom > display.bottom ? -Math.min(inoutObj.bottom - display.bottom, inoutObj.top - display.top) : 0;
            inoutObj.offset(0, dy);
        }
        if ((gravity & DISPLAY_CLIP_HORIZONTAL) != 0) {
            inoutObj.left = Math.max(inoutObj.left, display.left);
            inoutObj.right = Math.min(inoutObj.right, display.right);
        } else {
            int dx = inoutObj.left < display.left ? display.left - inoutObj.left
                : inoutObj.right > display.right ? -Math.min(inoutObj.right - display.right, inoutObj.left - display.left) : 0;
            inoutObj.offset(dx, 0);
        }
    }

    public static void applyDisplay(int gravity, android.graphics.Rect display, android.graphics.Rect inoutObj, int layoutDirection) {
        applyDisplay(getAbsoluteGravity(gravity, layoutDirection), display, inoutObj);
    }
}
