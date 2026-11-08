// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.PixelFormat;

/** A rectangle of one color. */
public class ColorDrawable extends Drawable {
    private int baseColor, useColor;
    private ColorFilter filter;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public ColorDrawable() {}
    public ColorDrawable(int color) { setColor(color); }

    @Override
    public void draw(Canvas canvas) {
        if ((useColor >>> 24) != 0 || filter != null) {
            paint.setColor(useColor);
            paint.setColorFilter(filter);
            canvas.drawRect(getBounds(), paint);
        }
    }

    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, new int[] {0x010101a5}); // color
        try {
            setColor(a.getColor(0, baseColor));
        } finally {
            a.recycle();
        }
    }
    public int getColor() { return useColor; }
    public void setColor(int color) {
        if (baseColor != color || useColor != color) {
            baseColor = useColor = color;
            invalidateSelf();
        }
    }
    @Override public int getAlpha() { return useColor >>> 24; }
    @Override public void setAlpha(int alpha) {
        int base = baseColor >>> 24;
        useColor = (baseColor & 0xffffff) | ((base * (alpha + (alpha >> 7)) >> 8) << 24);
        invalidateSelf();
    }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() {
        int a = useColor >>> 24;
        return a == 255 ? PixelFormat.OPAQUE : a == 0 ? PixelFormat.TRANSPARENT : PixelFormat.TRANSLUCENT;
    }
    @Override public void getOutline(Outline outline) {
        outline.setRect(getBounds());
        outline.setAlpha((useColor >>> 24) / 255f);
    }
    @Override public ConstantState getConstantState() {
        final int color = baseColor;
        return new ConstantState() {
            @Override public Drawable newDrawable() { return new ColorDrawable(color); }
            @Override public int getChangingConfigurations() { return 0; }
        };
    }
}
