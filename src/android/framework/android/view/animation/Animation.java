// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** An animation of a view's matrix and alpha over time, which the view applies on each frame. */
public abstract class Animation implements Cloneable {
    public static final int INFINITE = -1, RESTART = 1, REVERSE = 2, START_ON_FIRST_FRAME = -1, ABSOLUTE = 0,
        RELATIVE_TO_SELF = 1, RELATIVE_TO_PARENT = 2, ZORDER_NORMAL = 0, ZORDER_TOP = 1, ZORDER_BOTTOM = -1;
    static final String ANDROID = "http://schemas.android.com/apk/res/android";
    long startTime = START_ON_FIRST_FRAME, startOffset, duration;
    private int repeatCount, repeatMode = RESTART, repeated, zAdjustment, backgroundColor;
    private boolean fillBefore = true, fillAfter, fillEnabled, started, ended, initialized, cancelled, cycleFlip;
    private Interpolator interpolator;
    private AnimationListener listener;

    public interface AnimationListener {
        void onAnimationStart(Animation animation);
        void onAnimationEnd(Animation animation);
        void onAnimationRepeat(Animation animation);
    }

    public Animation() { ensureInterpolator(); }
    public Animation(Context context, AttributeSet attrs) {
        if (attrs != null) {
            duration = attrs.getAttributeIntValue(ANDROID, "duration", 0);
            startOffset = attrs.getAttributeIntValue(ANDROID, "startOffset", 0);
            fillEnabled = attrs.getAttributeBooleanValue(ANDROID, "fillEnabled", false);
            fillBefore = attrs.getAttributeBooleanValue(ANDROID, "fillBefore", true);
            fillAfter = attrs.getAttributeBooleanValue(ANDROID, "fillAfter", false);
            repeatCount = attrs.getAttributeIntValue(ANDROID, "repeatCount", 0);
            repeatMode = attrs.getAttributeIntValue(ANDROID, "repeatMode", RESTART);
            zAdjustment = attrs.getAttributeIntValue(ANDROID, "zAdjustment", ZORDER_NORMAL);
            int id = attrs.getAttributeResourceValue(ANDROID, "interpolator", 0);
            if (id != 0 && context != null) setInterpolator(context, id);
        }
        ensureInterpolator();
    }

    public void reset() { initialized = false; cycleFlip = false; repeated = 0; started = false; ended = false; cancelled = false; startTime = START_ON_FIRST_FRAME; }
    public void cancel() {
        if (started && !ended) {
            ended = true;
            if (listener != null) listener.onAnimationEnd(this);
        }
        cancelled = true;
        startTime = Long.MIN_VALUE;
    }
    public boolean isInitialized() { return initialized; }
    public void initialize(int width, int height, int parentWidth, int parentHeight) { reset(); initialized = true; }
    public void setInterpolator(Context context, int resID) { setInterpolator(AnimationUtils.loadInterpolator(context, resID)); }
    public void setInterpolator(Interpolator i) { interpolator = i; }
    public Interpolator getInterpolator() { return interpolator; }
    protected void ensureInterpolator() { if (interpolator == null) interpolator = new AccelerateDecelerateInterpolator(); }
    public void setStartOffset(long startOffset) { this.startOffset = startOffset; }
    public long getStartOffset() { return startOffset; }
    public void setDuration(long durationMillis) {
        if (durationMillis < 0) throw new IllegalArgumentException("Animation duration cannot be negative");
        duration = durationMillis;
    }
    public long getDuration() { return duration; }
    public void restrictDuration(long durationMillis) {
        if (startOffset > durationMillis) { startOffset = durationMillis; duration = 0; repeatCount = 0; return; }
        long d = duration + startOffset;
        if (d > durationMillis) duration = d = durationMillis - startOffset;
        if (duration <= 0) { duration = 0; repeatCount = 0; return; }
        if (repeatCount < 0 || repeatCount > durationMillis || d * repeatCount > durationMillis) {
            repeatCount = (int) (durationMillis / d) - 1;
            if (repeatCount < 0) repeatCount = 0;
        }
    }
    public void scaleCurrentDuration(float scale) { duration = (long) (duration * scale); startOffset = (long) (startOffset * scale); }
    public void setStartTime(long startTimeMillis) { startTime = startTimeMillis; started = ended = false; cycleFlip = false; repeated = 0; }
    public long getStartTime() { return startTime; }
    public void start() { setStartTime(-1); }
    public void startNow() { setStartTime(AnimationUtils.currentAnimationTimeMillis()); }
    public void setRepeatMode(int repeatMode) { this.repeatMode = repeatMode; }
    public int getRepeatMode() { return repeatMode; }
    public void setRepeatCount(int repeatCount) { this.repeatCount = repeatCount < 0 ? INFINITE : repeatCount; }
    public int getRepeatCount() { return repeatCount; }
    public void setFillEnabled(boolean fillEnabled) { this.fillEnabled = fillEnabled; }
    public boolean isFillEnabled() { return fillEnabled; }
    public void setFillBefore(boolean fillBefore) { this.fillBefore = fillBefore; }
    public boolean getFillBefore() { return fillBefore; }
    public void setFillAfter(boolean fillAfter) { this.fillAfter = fillAfter; }
    public boolean getFillAfter() { return fillAfter; }
    public void setZAdjustment(int zAdjustment) { this.zAdjustment = zAdjustment; }
    public int getZAdjustment() { return zAdjustment; }
    public void setBackgroundColor(int bg) { backgroundColor = bg; }
    public int getBackgroundColor() { return backgroundColor; }
    public void setDetachWallpaper(boolean detachWallpaper) {}
    public boolean getDetachWallpaper() { return false; }
    public boolean willChangeTransformationMatrix() { return true; }
    public boolean willChangeBounds() { return true; }
    public void setAnimationListener(AnimationListener listener) { this.listener = listener; }
    public long computeDurationHint() { return (startOffset + duration) * (repeatCount < 0 ? 1 : repeatCount + 1); }
    public boolean hasStarted() { return started; }
    public boolean hasEnded() { return ended; }

    /** The transformation at a time. False when the animation has ended. */
    public boolean getTransformation(long currentTime, Transformation outTransformation) {
        if (startTime == START_ON_FIRST_FRAME) startTime = currentTime;
        long start = startTime + startOffset;
        float t = duration != 0 ? (currentTime - start) / (float) duration : currentTime < start ? 0 : 1;
        boolean expired = t >= 1 || cancelled;
        boolean more = !expired;
        if (!fillEnabled) t = Math.max(Math.min(t, 1), 0);
        if ((t >= 0 || fillBefore) && (t <= 1 || fillAfter)) {
            if (!started) {
                started = true;
                if (listener != null) listener.onAnimationStart(this);
            }
            if (fillEnabled) t = Math.max(Math.min(t, 1), 0);
            if (cycleFlip) t = 1 - t;
            applyTransformation(interpolator.getInterpolation(t), outTransformation);
        }
        if (expired) {
            if (repeatCount == repeated || cancelled) {
                if (!ended) {
                    ended = true;
                    if (listener != null) listener.onAnimationEnd(this);
                }
            } else {
                if (repeatCount > 0) ++repeated;
                if (repeatMode == REVERSE) cycleFlip = !cycleFlip;
                startTime = START_ON_FIRST_FRAME;
                more = true;
                if (listener != null) listener.onAnimationRepeat(this);
            }
        }
        return more;
    }
    public boolean getTransformation(long currentTime, Transformation outTransformation, float scale) {
        return getTransformation(currentTime, outTransformation);
    }
    protected void applyTransformation(float interpolatedTime, Transformation t) {}
    protected float resolveSize(int type, float value, int size, int parentSize) {
        switch (type) {
            case RELATIVE_TO_SELF: return size * value;
            case RELATIVE_TO_PARENT: return parentSize * value;
            default: return value;
        }
    }
    protected float getScaleFactor() { return 1; }
    @Override protected Animation clone() throws CloneNotSupportedException {
        Animation a = (Animation) super.clone();
        a.reset();
        return a;
    }
}
