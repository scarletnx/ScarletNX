// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A display's rounded corner. */
public final class RoundedCorner {
    public static final int POSITION_TOP_LEFT = 0, POSITION_TOP_RIGHT = 1, POSITION_BOTTOM_RIGHT = 2, POSITION_BOTTOM_LEFT = 3;
    public int getRadius() { return 0; }
    public int getPosition() { return 0; }
    public android.graphics.Point getCenter() { return new android.graphics.Point(); }
}
