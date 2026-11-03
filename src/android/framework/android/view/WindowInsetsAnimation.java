// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Insets;
import java.util.List;

/** An animation of insets (the keyboard's): none here. */
public final class WindowInsetsAnimation {
    private final int typeMask;
    private final long duration;
    private final android.view.animation.Interpolator interpolator;
    private float fraction, alpha = 1;

    public WindowInsetsAnimation(int typeMask, android.view.animation.Interpolator interpolator, long durationMillis) {
        this.typeMask = typeMask;
        this.interpolator = interpolator;
        duration = durationMillis;
    }
    public android.view.animation.Interpolator getInterpolator() { return interpolator; }
    public int getTypeMask() { return typeMask; }
    public float getFraction() { return fraction; }
    public float getInterpolatedFraction() { return interpolator != null ? interpolator.getInterpolation(fraction) : fraction; }
    public void setFraction(float fraction) { this.fraction = fraction; }
    public long getDurationMillis() { return duration; }
    public float getAlpha() { return alpha; }
    public void setAlpha(float alpha) { this.alpha = alpha; }

    public static final class Bounds {
        private final Insets lower, upper;
        public Bounds(Insets lowerBound, Insets upperBound) { lower = lowerBound; upper = upperBound; }
        public Insets getLowerBound() { return lower; }
        public Insets getUpperBound() { return upper; }
        public Bounds inset(Insets insets) { return this; }
    }

    public abstract static class Callback {
        public static final int DISPATCH_MODE_STOP = 0, DISPATCH_MODE_CONTINUE_ON_SUBTREE = 1;
        private final int mode;
        public Callback(int dispatchMode) { mode = dispatchMode; }
        public final int getDispatchMode() { return mode; }
        public void onPrepare(WindowInsetsAnimation animation) {}
        public Bounds onStart(WindowInsetsAnimation animation, Bounds bounds) { return bounds; }
        public abstract WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> runningAnimations);
        public void onEnd(WindowInsetsAnimation animation) {}
    }
}
