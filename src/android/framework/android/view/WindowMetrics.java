// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Rect;

/** A window's bounds and insets. */
public final class WindowMetrics {
    private final Rect bounds;
    private final WindowInsets insets;

    public WindowMetrics(Rect bounds, WindowInsets windowInsets) { this.bounds = bounds; insets = windowInsets; }
    public WindowMetrics(Rect bounds, WindowInsets windowInsets, float density) { this(bounds, windowInsets); }
    static WindowMetrics current() {
        android.util.DisplayMetrics m = snx.android.Device.metrics();
        return new WindowMetrics(new Rect(0, 0, m.widthPixels, m.heightPixels), WindowInsets.none());
    }
    public Rect getBounds() { return bounds; }
    public WindowInsets getWindowInsets() { return insets; }
    public float getDensity() { return snx.android.Device.metrics().density; }
}
