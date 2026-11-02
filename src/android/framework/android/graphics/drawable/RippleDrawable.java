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
 */
public class RippleDrawable extends Drawable {
    public static final int RADIUS_AUTO = -1;
    private static final int PRESSED = 0x010100a7, FOCUSED = 0x0101009c, HOVERED = 0x01010367, ENABLED = 0x0101009e;

    private ColorStateList color;
    private final Drawable content, mask;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float hotspotX, hotspotY;
    private boolean hotspot, active;
    private int radius = RADIUS_AUTO, alpha = 255;

    public RippleDrawable(ColorStateList color, Drawable content, Drawable mask) {
        this.color = color;
        this.content = content;
        this.mask = mask;
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
        hotspot = true;
        hotspotX = x;
        hotspotY = y;
        if (active) invalidateSelf();
    }

    @Override protected boolean onStateChange(int[] state) {
        boolean pressed = false, enabled = false;
        for (int s : state) {
            if (s == PRESSED || s == FOCUSED || s == HOVERED) pressed = true;
            if (s == ENABLED) enabled = true;
        }
        boolean now = pressed && enabled;
        if (content != null) content.setState(state);
        if (now == active) return false;
        active = now;
        invalidateSelf();
        return true;
    }

    @Override protected void onBoundsChange(Rect bounds) {
        if (content != null) content.setBounds(bounds);
        if (mask != null) mask.setBounds(bounds);
    }

    @Override public void draw(Canvas canvas) {
        if (content != null) content.draw(canvas);
        if (!active || color == null) return;
        Rect b = getBounds();
        int c = color.getColorForState(getState(), color.getDefaultColor());
        paint.setColor(c);
        paint.setAlpha((c >>> 24) * alpha / 255);
        float x = hotspot ? hotspotX : b.exactCenterX(), y = hotspot ? hotspotY : b.exactCenterY();
        // the radius that reaches the bounds' farthest corner from the hotspot, as the auto radius does
        float r = radius != RADIUS_AUTO ? radius : (float) Math.hypot(Math.max(x - b.left, b.right - x), Math.max(y - b.top, b.bottom - y));
        int save = canvas.save();
        canvas.clipRect(b);
        canvas.drawCircle(x, y, r, paint);
        canvas.restoreToCount(save);
    }

    @Override public void setAlpha(int alpha) { this.alpha = alpha; invalidateSelf(); }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { paint.setColorFilter(colorFilter); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
