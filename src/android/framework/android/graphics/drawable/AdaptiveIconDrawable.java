// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;

/**
 * A launcher icon of two layers. Each layer is 108 by 108 units, and the icon shows the 72 units in the middle of
 * them, inside a round mask.
 */
public class AdaptiveIconDrawable extends Drawable implements Drawable.Callback {
    private static final float EXTRA_INSET_PERCENTAGE = 1 / 4f;
    private final Drawable background, foreground;
    private final Rect layer = new Rect();
    private final Path mask = new Path();
    private int alpha = 0xff;

    public AdaptiveIconDrawable(Drawable backgroundDrawable, Drawable foregroundDrawable) { this(backgroundDrawable, foregroundDrawable, null); }
    public AdaptiveIconDrawable(Drawable backgroundDrawable, Drawable foregroundDrawable, Drawable monochromeDrawable) {
        background = backgroundDrawable;
        foreground = foregroundDrawable;
        if (background != null) background.setCallback(this);
        if (foreground != null) foreground.setCallback(this);
    }
    public static float getExtraInsetFraction() { return EXTRA_INSET_PERCENTAGE; }
    public Drawable getBackground() { return background; }
    public Drawable getForeground() { return foreground; }
    public Drawable getMonochrome() { return null; }
    public Path getIconMask() { return mask; }

    @Override protected void onBoundsChange(Rect bounds) {
        int dx = (int) (bounds.width() * EXTRA_INSET_PERCENTAGE), dy = (int) (bounds.height() * EXTRA_INSET_PERCENTAGE);
        layer.set(bounds.left - dx, bounds.top - dy, bounds.right + dx, bounds.bottom + dy);
        if (background != null) background.setBounds(layer);
        if (foreground != null) foreground.setBounds(layer);
        mask.reset();
        mask.addOval(bounds.left, bounds.top, bounds.right, bounds.bottom, Path.Direction.CW);
    }
    @Override public void draw(Canvas canvas) {
        canvas.save();
        canvas.clipPath(mask);
        if (background != null) background.draw(canvas);
        if (foreground != null) foreground.draw(canvas);
        canvas.restore();
    }
    @Override public void setAlpha(int alpha) {
        this.alpha = alpha;
        if (background != null) background.setAlpha(alpha);
        if (foreground != null) foreground.setAlpha(alpha);
    }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) {
        if (background != null) background.setColorFilter(colorFilter);
        if (foreground != null) foreground.setColorFilter(colorFilter);
    }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    @Override public boolean isStateful() { return (background != null && background.isStateful()) || (foreground != null && foreground.isStateful()); }
    @Override protected boolean onStateChange(int[] state) {
        boolean changed = background != null && background.setState(state);
        return (foreground != null && foreground.setState(state)) || changed;
    }
    @Override public int getIntrinsicWidth() {
        int w = Math.max(background != null ? background.getIntrinsicWidth() : -1, foreground != null ? foreground.getIntrinsicWidth() : -1);
        return w < 0 ? -1 : (int) (w / (1 + 2 * EXTRA_INSET_PERCENTAGE));
    }
    @Override public int getIntrinsicHeight() {
        int h = Math.max(background != null ? background.getIntrinsicHeight() : -1, foreground != null ? foreground.getIntrinsicHeight() : -1);
        return h < 0 ? -1 : (int) (h / (1 + 2 * EXTRA_INSET_PERCENTAGE));
    }

    @Override public void invalidateDrawable(Drawable who) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }
}
