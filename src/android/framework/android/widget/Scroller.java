// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.hardware.SensorManager;
import android.view.ViewConfiguration;
import android.view.animation.AnimationUtils;
import android.view.animation.Interpolator;

/** The positions of a scroll or a fling over time. The view asks for them on each frame. */
public class Scroller {
    private final ScrollAxis x = new ScrollAxis(), y = new ScrollAxis();
    private final Interpolator interpolator;
    private final float ppi;
    private float deceleration;

    public Scroller(Context context) { this(context, null); }
    public Scroller(Context context, Interpolator interpolator) { this(context, interpolator, true); }
    public Scroller(Context context, Interpolator interpolator, boolean flywheel) {
        this.interpolator = interpolator;
        ppi = context.getResources().getDisplayMetrics().density * 160f;
        setFriction(ViewConfiguration.getScrollFriction());
    }

    /** The deceleration is the friction against gravity, in pixels per second squared. */
    public final void setFriction(float friction) { deceleration = SensorManager.GRAVITY_EARTH * 39.37f * ppi * friction; }
    public final boolean isFinished() { return x.finished && y.finished; }
    public final void forceFinished(boolean finished) { x.finished = finished; y.finished = finished; }
    public final int getDuration() { return Math.max(x.duration, y.duration); }
    public final int getCurrX() { return x.curr; }
    public final int getCurrY() { return y.curr; }
    public float getCurrVelocity() { long now = AnimationUtils.currentAnimationTimeMillis(); return (float) Math.hypot(x.velocity(now), y.velocity(now)); }
    public final int getStartX() { return x.start; }
    public final int getStartY() { return y.start; }
    public final int getFinalX() { return x.fin; }
    public final int getFinalY() { return y.fin; }
    public boolean computeScrollOffset() {
        if (isFinished()) return false;
        long now = AnimationUtils.currentAnimationTimeMillis();
        x.update(now, interpolator);
        y.update(now, interpolator);
        return true;
    }
    public void startScroll(int startX, int startY, int dx, int dy) { startScroll(startX, startY, dx, dy, 250); }
    public void startScroll(int startX, int startY, int dx, int dy, int duration) {
        long now = AnimationUtils.currentAnimationTimeMillis();
        x.startScroll(startX, dx, duration, now);
        y.startScroll(startY, dy, duration, now);
    }
    public void fling(int startX, int startY, int velocityX, int velocityY, int minX, int maxX, int minY, int maxY) {
        long now = AnimationUtils.currentAnimationTimeMillis();
        double v = Math.hypot(velocityX, velocityY);
        // the deceleration acts against the whole velocity, so each axis gets its part of it
        float ax = v > 0 ? (float) (deceleration * Math.abs(velocityX) / v) : deceleration;
        float ay = v > 0 ? (float) (deceleration * Math.abs(velocityY) / v) : deceleration;
        x.fling(startX, velocityX, minX, maxX, Math.max(ax, 1e-3f), now);
        y.fling(startY, velocityY, minY, maxY, Math.max(ay, 1e-3f), now);
    }
    public void abortAnimation() { x.abort(); y.abort(); }
    public void extendDuration(int extend) { long now = AnimationUtils.currentAnimationTimeMillis(); x.extend(extend, now); y.extend(extend, now); }
    public int timePassed() { return x.elapsed(AnimationUtils.currentAnimationTimeMillis()); }
    public void setFinalX(int newX) { x.setFinal(newX); }
    public void setFinalY(int newY) { y.setFinal(newY); }
}
