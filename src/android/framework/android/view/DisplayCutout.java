// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Insets;
import android.graphics.Rect;
import java.util.Collections;
import java.util.List;

/** A display's cutout: the window never has one. */
public final class DisplayCutout {
    public DisplayCutout(Insets safeInsets, Rect boundLeft, Rect boundTop, Rect boundRight, Rect boundBottom) {}
    public DisplayCutout(Insets safeInsets, Rect boundLeft, Rect boundTop, Rect boundRight, Rect boundBottom, Insets waterfallInsets) {}
    @Deprecated public DisplayCutout(Rect safeInsets, java.util.List<Rect> boundingRects) {}
    public int getSafeInsetTop() { return 0; }
    public int getSafeInsetBottom() { return 0; }
    public int getSafeInsetLeft() { return 0; }
    public int getSafeInsetRight() { return 0; }
    public List<Rect> getBoundingRects() { return Collections.emptyList(); }
    public Rect getBoundingRectTop() { return new Rect(); }
    public Rect getBoundingRectBottom() { return new Rect(); }
    public Rect getBoundingRectLeft() { return new Rect(); }
    public Rect getBoundingRectRight() { return new Rect(); }
    public Insets getWaterfallInsets() { return Insets.NONE; }
}
