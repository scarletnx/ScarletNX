// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;

/**
 * A touch feedback (Abstract in docs/android/INTERFACES.md): while the state is pressed, focused or hovered, a circle
 * of the ripple's color around the hotspot, over the content. Android animates the ripple in and out; this draws its
 * final frame, so the app sees the same states and invalidations without the animation.
 *
 * As on Android it is a LayerDrawable: the content is its first layer, and the mask a layer whose id is
 * android.R.id.mask, which bounds the ripple and is not drawn itself.
 */
public class RippleDrawable extends LayerDrawable {
    public static final int RADIUS_AUTO = -1;
    private static final int MASK_ID = 0x0102002e;
    private static final int PRESSED = 0x010100a7, FOCUSED = 0x0101009c, HOVERED = 0x01010367, ENABLED = 0x0101009e;

    private ColorStateList color;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float hotspotX, hotspotY;
    private boolean hotspot, active;
    private int radius = RADIUS_AUTO, rippleAlpha = 255;

    RippleDrawable() { super(new Drawable[0]); }
    @Override LayerDrawable newEmpty() { return new RippleDrawable(); }
    @Override void copyStateFrom(LayerDrawable from) {
        RippleDrawable r = (RippleDrawable) from;
        color = r.color;
        radius = r.radius;
    }

    /** a ripple's android:color and android:radius */
    private static final int[] RIPPLE_ATTRS = {0x010101a5, 0x010101a8};

    /** a ripple: its color and radius, and its items as a layer list's (the mask the one with android:id/mask) */
    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, RIPPLE_ATTRS);
        try {
            ColorStateList c = a.getColorStateList(0);
            if (c != null) color = c;
            radius = a.getDimensionPixelSize(1, radius);
        } finally {
            a.recycle();
        }
        super.inflate(r, parser, attrs, theme);
    }

    public RippleDrawable(ColorStateList color, Drawable content, Drawable mask) {
        super(content != null ? new Drawable[] {content} : new Drawable[0]);
        this.color = color;
        if (mask != null) setId(addLayer(mask), MASK_ID);
    }

    public void setColor(ColorStateList color) {
        this.color = color;
        invalidateSelf();
    }
    public void setRadius(int radius) {
        this.radius = radius;
        invalidateSelf();
    }
    public int getRadius() { return radius; }
    @Override public Rect getDirtyBounds() { return getBounds(); }
    @Override public boolean isStateful() { return true; }
    @Override public void setHotspot(float x, float y) {
        super.setHotspot(x, y);
        hotspot = true;
        hotspotX = x;
        hotspotY = y;
        if (active) invalidateSelf();
    }

    @Override protected boolean onStateChange(int[] state) {
        boolean changed = super.onStateChange(state);
        boolean pressed = false, enabled = false;
        for (int s : state) {
            if (s == PRESSED || s == FOCUSED || s == HOVERED) pressed = true;
            if (s == ENABLED) enabled = true;
        }
        boolean now = pressed && enabled;
        if (now == active) return changed;
        active = now;
        invalidateSelf();
        return true;
    }

    @Override public void draw(Canvas canvas) {
        for (int i = 0, n = getNumberOfLayers(); i < n; ++i) {
            Drawable d = getDrawable(i);
            if (d != null && getId(i) != MASK_ID) d.draw(canvas);
        }
        if (!active || color == null) return;
        Rect b = getBounds();
        int c = color.getColorForState(getState(), color.getDefaultColor());
        paint.setColor(c);
        paint.setAlpha((c >>> 24) * rippleAlpha / 255);
        float x = hotspot ? hotspotX : b.exactCenterX(), y = hotspot ? hotspotY : b.exactCenterY();
        // the radius that reaches the bounds' farthest corner from the hotspot, as the auto radius does
        float r = radius != RADIUS_AUTO ? radius : (float) Math.hypot(Math.max(x - b.left, b.right - x), Math.max(y - b.top, b.bottom - y));
        int save = canvas.save();
        canvas.clipRect(b);
        canvas.drawCircle(x, y, r, paint);
        canvas.restoreToCount(save);
    }

    @Override public void setAlpha(int alpha) { super.setAlpha(alpha); rippleAlpha = alpha; invalidateSelf(); }
    @Override public void setColorFilter(ColorFilter colorFilter) { super.setColorFilter(colorFilter); paint.setColorFilter(colorFilter); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
