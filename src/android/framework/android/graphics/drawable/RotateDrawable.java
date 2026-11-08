// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.Rect;

/**
 * A drawable turned by its level, as Android's: from fromDegrees at level 0 to toDegrees at 10000, about its pivot
 * (a fraction of the bounds by default, the middle).
 */
public class RotateDrawable extends DrawableWrapper {
    private static final int MAX_LEVEL = 10000;
    private float fromDegrees, toDegrees = 360, currentDegrees, pivotX = 0.5f, pivotY = 0.5f;
    private boolean pivotXRelative = true, pivotYRelative = true;

    public RotateDrawable() { super(null); }

    /** android:drawable, fromDegrees, toDegrees, pivotX and pivotY (a fraction, or a dimension) */
    private static final int[] ROTATE_ATTRS = {0x01010199, 0x010101b3, 0x010101b4, 0x010101b5, 0x010101b6};

    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, ROTATE_ATTRS);
        try {
            fromDegrees = a.getFloat(1, 0);
            toDegrees = a.getFloat(2, 360);
            currentDegrees = fromDegrees;
            android.util.TypedValue v = a.peekValue(3);
            if (v != null) {
                pivotXRelative = v.type == android.util.TypedValue.TYPE_FRACTION;
                pivotX = pivotXRelative ? v.getFraction(1, 1) : v.getFloat();
            }
            v = a.peekValue(4);
            if (v != null) {
                pivotYRelative = v.type == android.util.TypedValue.TYPE_FRACTION;
                pivotY = pivotYRelative ? v.getFraction(1, 1) : v.getFloat();
            }
            setDrawable(DrawableXml.itemDrawable(r, parser, attrs, theme, a, 0));
        } finally {
            a.recycle();
        }
    }

    @Override public void draw(Canvas canvas) {
        Drawable d = getDrawable();
        if (d == null) return;
        Rect b = d.getBounds();
        float px = pivotXRelative ? b.width() * pivotX : pivotX, py = pivotYRelative ? b.height() * pivotY : pivotY;
        int save = canvas.save();
        canvas.rotate(currentDegrees, px + b.left, py + b.top);
        d.draw(canvas);
        canvas.restoreToCount(save);
    }
    @Override protected boolean onLevelChange(int level) {
        super.onLevelChange(level);
        currentDegrees = fromDegrees + (toDegrees - fromDegrees) * level / (float) MAX_LEVEL;
        invalidateSelf();
        return true;
    }
    public void setFromDegrees(float fromDegrees) { this.fromDegrees = fromDegrees; invalidateSelf(); }
    public float getFromDegrees() { return fromDegrees; }
    public void setToDegrees(float toDegrees) { this.toDegrees = toDegrees; invalidateSelf(); }
    public float getToDegrees() { return toDegrees; }
    public void setPivotX(float pivotX) { this.pivotX = pivotX; invalidateSelf(); }
    public float getPivotX() { return pivotX; }
    public void setPivotY(float pivotY) { this.pivotY = pivotY; invalidateSelf(); }
    public float getPivotY() { return pivotY; }
    public void setPivotXRelative(boolean relative) { pivotXRelative = relative; invalidateSelf(); }
    public boolean isPivotXRelative() { return pivotXRelative; }
    public void setPivotYRelative(boolean relative) { pivotYRelative = relative; invalidateSelf(); }
    public boolean isPivotYRelative() { return pivotYRelative; }
}
