// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.hardware.SensorManager;
import android.view.ViewConfiguration;
import android.view.animation.AnimationUtils;
import android.view.animation.Interpolator;

/** A Scroller that also springs back into its bounds when a scroll ends outside them. */
public class OverScroller {
    private static final int SPRING_DURATION = 250;
    private final ScrollAxis x = new ScrollAxis(), y = new ScrollAxis();
    private final Interpolator interpolator;
    private final float deceleration;
    private int minX, maxX = Integer.MAX_VALUE, minY, maxY = Integer.MAX_VALUE;

    public OverScroller(Context context) { this(context, null); }
    public OverScroller(Context context, Interpolator interpolator) { this(context, interpolator, true); }
    public OverScroller(Context context, Interpolator interpolator, boolean flywheel) {
        this.interpolator = interpolator;
        float ppi = context.getResources().getDisplayMetrics().density * 160f;
        deceleration = SensorManager.GRAVITY_EARTH * 39.37f * ppi * ViewConfiguration.getScrollFriction();
    }

    public void setFriction(float friction) {}
    public final boolean isFinished() { return x.finished && y.finished; }
    public final void forceFinished(boolean finished) { x.finished = finished; y.finished = finished; }
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
    /** Starts a scroll back into the bounds from a position outside them. It returns false when the position is inside. */
    public boolean springBack(int startX, int startY, int minX, int maxX, int minY, int maxY) {
        long now = AnimationUtils.currentAnimationTimeMillis();
        boolean sx = spring(x, startX, minX, maxX, now), sy = spring(y, startY, minY, maxY, now);
        this.minX = minX; this.maxX = maxX; this.minY = minY; this.maxY = maxY;
        return sx || sy;
    }
    private static boolean spring(ScrollAxis a, int start, int min, int max, long now) {
        int target = Math.max(min, Math.min(max, start));
        if (target == start) { a.curr = a.start = a.fin = start; a.finished = true; return false; }
        a.startScroll(start, target - start, SPRING_DURATION, now);
        return true;
    }
    public void fling(int startX, int startY, int velocityX, int velocityY, int minX, int maxX, int minY, int maxY) {
        fling(startX, startY, velocityX, velocityY, minX, maxX, minY, maxY, 0, 0);
    }
    /** A fling stops at its bounds: the over-scroll range is not used, as no edge glow follows the fling here. */
    public void fling(int startX, int startY, int velocityX, int velocityY, int minX, int maxX, int minY, int maxY, int overX, int overY) {
        long now = AnimationUtils.currentAnimationTimeMillis();
        this.minX = minX; this.maxX = maxX; this.minY = minY; this.maxY = maxY;
        double v = Math.hypot(velocityX, velocityY);
        float ax = v > 0 ? (float) (deceleration * Math.abs(velocityX) / v) : deceleration;
        float ay = v > 0 ? (float) (deceleration * Math.abs(velocityY) / v) : deceleration;
        if (startX < minX || startX > maxX) spring(x, startX, minX, maxX, now); else x.fling(startX, velocityX, minX, maxX, Math.max(ax, 1e-3f), now);
        if (startY < minY || startY > maxY) spring(y, startY, minY, maxY, now); else y.fling(startY, velocityY, minY, maxY, Math.max(ay, 1e-3f), now);
    }
    public void notifyHorizontalEdgeReached(int startX, int finalX, int overX) { spring(x, startX, finalX, finalX, AnimationUtils.currentAnimationTimeMillis()); }
    public void notifyVerticalEdgeReached(int startY, int finalY, int overY) { spring(y, startY, finalY, finalY, AnimationUtils.currentAnimationTimeMillis()); }
    public boolean isOverScrolled() { return !x.finished && (x.curr < minX || x.curr > maxX) || !y.finished && (y.curr < minY || y.curr > maxY); }
    public void abortAnimation() { x.abort(); y.abort(); }
    public int timePassed() { long now = AnimationUtils.currentAnimationTimeMillis(); return Math.min(x.elapsed(now), y.elapsed(now)); }
    public boolean isScrollingInDirection(float xvel, float yvel) {
        int dx = x.fin - x.start, dy = y.fin - y.start;
        return !isFinished() && Math.signum(xvel) == Math.signum(dx) && Math.signum(yvel) == Math.signum(dy);
    }
}
