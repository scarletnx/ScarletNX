// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Rect;

/** A drawable drawn inside its bounds, inset by fixed distances. */
public class InsetDrawable extends DrawableWrapper {
    private int left, top, right, bottom;
    private final Rect tmp = new Rect();

    InsetDrawable() { super(null); }

    /** android:drawable, inset, insetLeft, insetRight, insetTop and insetBottom */
    private static final int[] INSET_ATTRS = {0x01010199, 0x010104b5, 0x010101b7, 0x010101b8, 0x010101b9, 0x010101ba};

    /** an inset: its drawable (the attribute or a child element) and its insets, each side's over the overall one */
    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, INSET_ATTRS);
        try {
            int all = (int) DrawableXml.dimensionOrFraction(a, 1, 0, 0);
            left = (int) DrawableXml.dimensionOrFraction(a, 2, 0, all);
            right = (int) DrawableXml.dimensionOrFraction(a, 3, 0, all);
            top = (int) DrawableXml.dimensionOrFraction(a, 4, 0, all);
            bottom = (int) DrawableXml.dimensionOrFraction(a, 5, 0, all);
            setDrawable(DrawableXml.itemDrawable(r, parser, attrs, theme, a, 0));
        } finally {
            a.recycle();
        }
    }

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
    /** a resource's, else one of a copy of the inset drawable with the same insets (none without the child's) */
    @Override public ConstantState getConstantState() {
        if (fromResource()) return super.getConstantState();
        Drawable child = getDrawable();
        final ConstantState inner = child != null ? child.getConstantState() : null;
        if (child != null && inner == null) return null;
        final int l = left, t = top, r = right, b = bottom;
        return new Copy() {
            @Override public Drawable newDrawable(android.content.res.Resources res, android.content.res.Resources.Theme theme) {
                return new InsetDrawable(inner != null ? inner.newDrawable(res, theme) : null, l, t, r, b);
            }
        };
    }
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
