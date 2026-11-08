// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.view.animation.Interpolator;

/**
 * The motion of one axis of a Scroller or an OverScroller: a scroll over a fixed time, or a fling that slows down at a
 * constant rate until it stops or reaches its bound.
 */
final class ScrollAxis {
    private static final int SCROLL = 0, FLING = 1;
    int start, fin, curr, duration;
    boolean finished = true;
    private int kind;
    private long startTime;
    private float speed, decel;
    private int sign;

    void startScroll(int start, int distance, int duration, long now) {
        kind = SCROLL;
        this.start = curr = start;
        fin = start + distance;
        this.duration = Math.max(0, duration);
        startTime = now;
        finished = false;
    }
    /** A fling from a velocity in pixels per second. It stops at the distance the deceleration gives, or at min or max. */
    void fling(int start, int velocity, int min, int max, float deceleration, long now) {
        kind = FLING;
        this.start = curr = start;
        startTime = now;
        sign = velocity < 0 ? -1 : 1;
        speed = Math.abs(velocity);
        decel = deceleration;
        double distance = speed * speed / (2.0 * decel);
        long target = start + sign * Math.round(distance);
        fin = (int) Math.max(min, Math.min(max, target));
        double d = Math.abs(fin - start);
        // the time at which the motion covers d: speed * t - decel * t^2 / 2 = d
        double t = speed > 0 ? (speed - Math.sqrt(Math.max(0, speed * speed - 2 * decel * d))) / decel : 0;
        duration = (int) Math.round(t * 1000);
        finished = duration == 0 && fin == start;
    }
    /** Moves curr to its position at a time. It returns false when the axis was already finished. */
    boolean update(long now, Interpolator interpolator) {
        if (finished) return false;
        long t = now - startTime;
        if (t >= duration) { curr = fin; finished = true; return true; }
        if (kind == SCROLL) {
            float x = (float) t / duration;
            float f = interpolator != null ? interpolator.getInterpolation(x) : 1 - (1 - x) * (1 - x) * (1 - x);
            curr = start + Math.round(f * (fin - start));
        } else {
            double s = t / 1000.0;
            curr = start + sign * (int) Math.round(speed * s - decel * s * s / 2);
        }
        return true;
    }
    float velocity(long now) {
        if (finished) return 0;
        if (kind == FLING) return sign * Math.max(0, speed - decel * (now - startTime) / 1000f);
        return duration > 0 ? (fin - start) * 1000f / duration : 0;
    }
    void abort() { curr = fin; finished = true; }
    int elapsed(long now) { return (int) (now - startTime); }
    void extend(int extend, long now) { duration = elapsed(now) + extend; finished = false; }
    void setFinal(int newFinal) { fin = newFinal; finished = false; }
}
