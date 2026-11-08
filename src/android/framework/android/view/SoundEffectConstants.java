// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Sound effect kinds. */
public class SoundEffectConstants {
    public static final int CLICK = 0, NAVIGATION_LEFT = 1, NAVIGATION_UP = 2, NAVIGATION_RIGHT = 3, NAVIGATION_DOWN = 4;
    public static int getContantForFocusDirection(int direction) { return CLICK; }
}
