// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;

/** A drawable of vector paths. The paths of a vector resource are not read yet, so it draws nothing. */
public class VectorDrawable extends Drawable {
    private int alpha = 0xff;
    private ColorFilter filter;

    public VectorDrawable() {}
    @Override public void draw(Canvas canvas) {}
    @Override public void setAlpha(int alpha) { this.alpha = alpha; }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
