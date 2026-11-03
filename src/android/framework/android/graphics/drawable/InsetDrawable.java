// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Rect;

/** A drawable drawn inside its bounds, inset by fixed distances. */
public class InsetDrawable extends DrawableWrapper {
    private final int left, top, right, bottom;
    private final Rect tmp = new Rect();

    public InsetDrawable(Drawable drawable, int inset) { this(drawable, inset, inset, inset, inset); }
    public InsetDrawable(Drawable drawable, float inset) { this(drawable, (int) inset); }
    public InsetDrawable(Drawable drawable, int insetLeft, int insetTop, int insetRight, int insetBottom) {
        super(drawable);
        left = insetLeft;
        top = insetTop;
        right = insetRight;
        bottom = insetBottom;
    }
    @Override public void draw(Canvas canvas) { super.draw(canvas); }
    @Override public Drawable getDrawable() { return super.getDrawable(); }
    @Override protected void onBoundsChange(Rect bounds) {
        tmp.set(bounds.left + left, bounds.top + top, bounds.right - right, bounds.bottom - bottom);
        super.onBoundsChange(tmp);
    }
    @Override public boolean getPadding(Rect padding) {
        boolean pad = super.getPadding(padding);
        padding.left += left;
        padding.top += top;
        padding.right += right;
        padding.bottom += bottom;
        return pad || (left | top | right | bottom) != 0;
    }
    @Override public Insets getOpticalInsets() {
        Insets i = super.getOpticalInsets();
        return Insets.of(i.left + left, i.top + top, i.right + right, i.bottom + bottom);
    }
    @Override public int getIntrinsicWidth() {
        int w = super.getIntrinsicWidth();
        return w < 0 ? -1 : w + left + right;
    }
    @Override public int getIntrinsicHeight() {
        int h = super.getIntrinsicHeight();
        return h < 0 ? -1 : h + top + bottom;
    }
}
