// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;
import java.util.ArrayList;

/**
 * A vector drawable with animators. It draws its vector as the animations start it (their first frame is not
 * applied), and its animation has no frames: a start tells the callbacks that the animation started, and a stop that
 * it ended.
 */
public class AnimatedVectorDrawable extends Drawable implements Animatable2 {
    private final ArrayList<AnimationCallback> callbacks = new ArrayList<>();
    private boolean running;
    private Drawable vector;

    public AnimatedVectorDrawable() {}

    /** an animated vector's android:drawable: the vector it animates (its targets are not played) */
    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, new int[] {0x01010199}); // drawable
        try {
            if (a.hasValue(0)) {
                vector = DrawableXml.drawableOf(r, a, 0, theme);
                if (vector != null) {
                    vector.setCallback(new Callback() {
                        @Override public void invalidateDrawable(Drawable who) { invalidateSelf(); }
                        @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
                        @Override public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }
                    });
                    vector.setBounds(getBounds());
                }
            }
        } finally {
            a.recycle();
        }
        DrawableXml.skip(parser);
    }

    @Override public void draw(Canvas canvas) { if (vector != null) vector.draw(canvas); }
    @Override protected void onBoundsChange(android.graphics.Rect bounds) { if (vector != null) vector.setBounds(bounds); }
    @Override protected boolean onStateChange(int[] state) { return vector != null && vector.setState(state); }
    @Override public boolean isStateful() { return vector != null && vector.isStateful(); }
    @Override public int getIntrinsicWidth() { return vector != null ? vector.getIntrinsicWidth() : -1; }
    @Override public int getIntrinsicHeight() { return vector != null ? vector.getIntrinsicHeight() : -1; }
    private int alpha = 0xff;
    private ColorFilter filter;
    @Override public void setAlpha(int alpha) { this.alpha = alpha; if (vector != null) vector.setAlpha(alpha); }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; if (vector != null) vector.setColorFilter(colorFilter); }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public void setTintList(android.content.res.ColorStateList tint) { if (vector != null) vector.setTintList(tint); }
    @Override public void setTintMode(android.graphics.PorterDuff.Mode mode) { if (vector != null) vector.setTintMode(mode); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    @Override public void start() {
        if (running) return;
        running = true;
        for (AnimationCallback c : new ArrayList<>(callbacks)) c.onAnimationStart(this);
    }
    @Override public void stop() {
        if (!running) return;
        running = false;
        for (AnimationCallback c : new ArrayList<>(callbacks)) c.onAnimationEnd(this);
    }
    public void reset() { running = false; }
    public void reverse() {}
    public boolean canReverse() { return false; }
    @Override public boolean isRunning() { return running; }
    @Override public void registerAnimationCallback(AnimationCallback callback) { if (callback != null && !callbacks.contains(callback)) callbacks.add(callback); }
    @Override public boolean unregisterAnimationCallback(AnimationCallback callback) { return callbacks.remove(callback); }
    @Override public void clearAnimationCallbacks() { callbacks.clear(); }
}
