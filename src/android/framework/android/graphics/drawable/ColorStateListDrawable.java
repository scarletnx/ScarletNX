// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.PixelFormat;

/** A rectangle of the color a color state list gives for the drawable's state. */
public class ColorStateListDrawable extends Drawable {
    private ColorStateList colors;
    private int alpha = 255;
    private ColorFilter filter;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public ColorStateListDrawable() { this(ColorStateList.valueOf(0)); }
    public ColorStateListDrawable(ColorStateList colorStateList) { colors = colorStateList; }

    /** the color for the current state, with the drawable's alpha */
    private int color() {
        int c = colors.getColorForState(getState(), colors.getDefaultColor());
        return (c & 0xffffff) | (((c >>> 24) * (alpha + (alpha >> 7)) >> 8) << 24);
    }
    @Override public void draw(Canvas canvas) {
        int c = color();
        if ((c >>> 24) != 0 || filter != null) {
            paint.setColor(c);
            paint.setColorFilter(filter);
            canvas.drawRect(getBounds(), paint);
        }
    }
    public ColorStateList getColorStateList() { return colors; }
    public void setColorStateList(ColorStateList colorStateList) { colors = colorStateList; invalidateSelf(); }
    @Override public boolean isStateful() { return colors.isStateful(); }
    @Override public boolean hasFocusStateSpecified() { return colors.isStateful(); }
    @Override protected boolean onStateChange(int[] state) { invalidateSelf(); return colors.isStateful(); }
    @Override public int getAlpha() { return alpha; }
    @Override public void setAlpha(int alpha) { this.alpha = alpha; invalidateSelf(); }
    public void clearAlpha() { setAlpha(255); }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() {
        int a = color() >>> 24;
        return a == 255 ? PixelFormat.OPAQUE : a == 0 ? PixelFormat.TRANSPARENT : PixelFormat.TRANSLUCENT;
    }
    @Override public void getOutline(Outline outline) {
        outline.setRect(getBounds());
        outline.setAlpha((color() >>> 24) / 255f);
    }
    @Override public ConstantState getConstantState() {
        final ColorStateList list = colors;
        return new ConstantState() {
            @Override public Drawable newDrawable() { return new ColorStateListDrawable(list); }
            @Override public int getChangingConfigurations() { return 0; }
        };
    }
}
