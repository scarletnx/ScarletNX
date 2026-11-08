// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import android.view.Choreographer;
import java.util.ArrayList;

/** Values animated over time on the Choreographer's frames. */
public class ValueAnimator extends Animator {
    public static final int RESTART = 1, REVERSE = 2, INFINITE = -1;
    private static float durationScale = 1;
    private long duration = 300, startDelay, startTime = -1;
    PropertyValuesHolder[] values;
    private TimeInterpolator interpolator = t -> (float) (Math.cos((t + 1) * Math.PI) / 2) + 0.5f;
    private float fraction;
    private boolean running;
    private int repeatCount, repeatMode = RESTART;
    private ArrayList<AnimatorUpdateListener> updateListeners = new ArrayList<>();
    private Choreographer.FrameCallback frame = this::doFrame;

    public interface AnimatorUpdateListener {
        void onAnimationUpdate(ValueAnimator animation);
    }

    public interface DurationScaleChangeListener {
        void onChanged(float scale);
    }

    public ValueAnimator() {}
    public static ValueAnimator ofFloat(float... values) { ValueAnimator a = new ValueAnimator(); a.setFloatValues(values); return a; }
    public static ValueAnimator ofInt(int... values) { ValueAnimator a = new ValueAnimator(); a.setIntValues(values); return a; }
    public static float getDurationScale() { return durationScale; }
    public static boolean areAnimatorsEnabled() { return durationScale != 0; }
    public static boolean registerDurationScaleChangeListener(DurationScaleChangeListener listener) { return true; }
    public static boolean unregisterDurationScaleChangeListener(DurationScaleChangeListener listener) { return true; }
    public static ValueAnimator ofArgb(int... values) { ValueAnimator a = ofInt(values); a.setEvaluator(ArgbEvaluator.getInstance()); return a; }
    @SuppressWarnings("rawtypes")
    public static ValueAnimator ofObject(TypeEvaluator evaluator, Object... values) {
        ValueAnimator a = new ValueAnimator();
        a.setObjectValues(values);
        a.setEvaluator(evaluator);
        return a;
    }
    public static ValueAnimator ofPropertyValuesHolder(PropertyValuesHolder... values) { ValueAnimator a = new ValueAnimator(); a.setValues(values); return a; }
    public void setFloatValues(float... v) { if (values == null || values.length == 0) setValues(PropertyValuesHolder.ofFloat("", v)); else values[0].setFloatValues(v); }
    public void setIntValues(int... v) { if (values == null || values.length == 0) setValues(PropertyValuesHolder.ofInt("", v)); else values[0].setIntValues(v); }
    public void setObjectValues(Object... v) {
        if (values == null || values.length == 0) setValues(PropertyValuesHolder.ofObject("", null, v));
        else values[0].setObjectValues(v);
    }
    public void setValues(PropertyValuesHolder... values) { this.values = values; }
    public PropertyValuesHolder[] getValues() { return values; }
    @SuppressWarnings("rawtypes")
    public void setEvaluator(TypeEvaluator value) { if (value != null && values != null && values.length > 0) values[0].setEvaluator(value); }
    @Override public ValueAnimator setDuration(long duration) { this.duration = duration; return this; }
    @Override public long getDuration() { return duration; }
    @Override public long getStartDelay() { return startDelay; }
    @Override public void setStartDelay(long startDelay) { this.startDelay = startDelay; }
    @Override public void setInterpolator(TimeInterpolator value) { interpolator = value != null ? value : t -> t; }
    @Override public TimeInterpolator getInterpolator() { return interpolator; }
    public void setRepeatCount(int value) { repeatCount = value; }
    public int getRepeatCount() { return repeatCount; }
    public void setRepeatMode(int value) { repeatMode = value; }
    public int getRepeatMode() { return repeatMode; }
    @Override public boolean isRunning() { return running; }
    public void addUpdateListener(AnimatorUpdateListener listener) { updateListeners.add(listener); }
    public void removeUpdateListener(AnimatorUpdateListener listener) { updateListeners.remove(listener); }
    public void removeAllUpdateListeners() { updateListeners.clear(); }
    public float getAnimatedFraction() { return fraction; }
    public Object getAnimatedValue() {
        float t = interpolator.getInterpolation(fraction);
        return values != null && values.length > 0 ? values[0].value(t) : t;
    }
    public Object getAnimatedValue(String propertyName) {
        float t = interpolator.getInterpolation(fraction);
        if (values != null) for (PropertyValuesHolder h : values) if (h.getPropertyName().equals(propertyName)) return h.value(t);
        return null;
    }
    /** The values at a fraction of the time, before the update listeners see them. */
    void animateValue(float fraction) {}
    public long getCurrentPlayTime() { return startTime < 0 ? 0 : android.os.SystemClock.uptimeMillis() - startTime; }
    public void setCurrentPlayTime(long playTime) { fraction = duration == 0 ? 1 : Math.min(1, playTime / (float) duration); }
    public void setCurrentFraction(float fraction) { this.fraction = fraction; }
    @Override public void start() {
        running = true;
        startTime = -1;
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) l.onAnimationStart(this);
        Choreographer.getInstance().postFrameCallback(frame);
    }
    private void doFrame(long frameTimeNanos) {
        if (!running) return;
        long now = frameTimeNanos / 1000000;
        if (startTime < 0) startTime = now + startDelay;
        long scaled = (long) (duration * durationScale);
        fraction = scaled <= 0 ? 1 : Math.max(0, Math.min(1, (now - startTime) / (float) scaled));
        animateValue(fraction);
        for (AnimatorUpdateListener l : new ArrayList<>(updateListeners)) l.onAnimationUpdate(this);
        if (fraction >= 1) end();
        else Choreographer.getInstance().postFrameCallback(frame);
    }
    @Override public void cancel() {
        if (!running) return;
        running = false;
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) { l.onAnimationCancel(this); l.onAnimationEnd(this); }
    }
    @Override public void end() {
        if (!running) return;
        running = false;
        fraction = 1;
        animateValue(fraction);
        for (AnimatorUpdateListener l : new ArrayList<>(updateListeners)) l.onAnimationUpdate(this);
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) l.onAnimationEnd(this);
    }
    public void reverse() { start(); }
    @Override public ValueAnimator clone() {
        ValueAnimator a = (ValueAnimator) super.clone();
        a.updateListeners = new ArrayList<>(updateListeners);
        if (listeners != null) a.listeners = new ArrayList<>(listeners);
        a.frame = a::doFrame;
        a.running = false;
        a.startTime = -1;
        if (values != null) {
            a.values = new PropertyValuesHolder[values.length];
            for (int i = 0; i < values.length; ++i) a.values[i] = values[i].clone();
        }
        return a;
    }
}
