// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** Animations of a layout's changes (shown at once here). */
public class LayoutTransition {
    public static final int CHANGE_APPEARING = 0, CHANGE_DISAPPEARING = 1, APPEARING = 2, DISAPPEARING = 3, CHANGING = 4;
    public void enableTransitionType(int transitionType) {}
    public void disableTransitionType(int transitionType) {}
    public void setDuration(long duration) {}
}
