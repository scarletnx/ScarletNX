// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;
import java.util.ArrayList;
import java.util.List;

/** Animations played together, their transformations composed. */
public class AnimationSet extends Animation {
    private final boolean shareInterpolator;
    private ArrayList<Animation> animations = new ArrayList<>();
    private boolean durationSet, fillAfterSet, fillBeforeSet, repeatModeSet, startOffsetSet;
    private final Transformation temp = new Transformation();
    private boolean setStarted, setEnded;
    private AnimationListener setListener;

    public AnimationSet(boolean shareInterpolator) { this.shareInterpolator = shareInterpolator; }
    public AnimationSet(Context context, AttributeSet attrs) {
        super(context, attrs);
        shareInterpolator = attrs == null || attrs.getAttributeBooleanValue(ANDROID, "shareInterpolator", true);
        if (attrs != null) {
            durationSet = attrs.getAttributeValue(ANDROID, "duration") != null;
            fillAfterSet = attrs.getAttributeValue(ANDROID, "fillAfter") != null;
            fillBeforeSet = attrs.getAttributeValue(ANDROID, "fillBefore") != null;
            repeatModeSet = attrs.getAttributeValue(ANDROID, "repeatMode") != null;
            startOffsetSet = attrs.getAttributeValue(ANDROID, "startOffset") != null;
        }
    }

    public void addAnimation(Animation a) { animations.add(a); }
    public List<Animation> getAnimations() { return animations; }
    @Override public void setDuration(long durationMillis) { super.setDuration(durationMillis); durationSet = true; }
    @Override public void setFillAfter(boolean fillAfter) { super.setFillAfter(fillAfter); fillAfterSet = true; }
    @Override public void setFillBefore(boolean fillBefore) { super.setFillBefore(fillBefore); fillBeforeSet = true; }
    @Override public void setRepeatMode(int repeatMode) { super.setRepeatMode(repeatMode); repeatModeSet = true; }
    @Override public void setStartOffset(long startOffset) { super.setStartOffset(startOffset); startOffsetSet = true; }
    @Override public void setAnimationListener(AnimationListener listener) { setListener = listener; }
    @Override public void setStartTime(long startTimeMillis) {
        super.setStartTime(startTimeMillis);
        for (Animation a : animations) a.setStartTime(startTimeMillis);
        setStarted = setEnded = false;
    }
    @Override public long getStartTime() {
        long t = Long.MAX_VALUE;
        for (Animation a : animations) t = Math.min(t, a.getStartTime());
        return animations.isEmpty() ? super.getStartTime() : t;
    }
    @Override public long getDuration() {
        if (durationSet) return duration;
        long d = 0;
        for (Animation a : animations) d = Math.max(d, a.getDuration());
        return d;
    }
    @Override public long computeDurationHint() {
        long d = 0;
        for (Animation a : animations) d = Math.max(d, a.computeDurationHint());
        return d;
    }
    @Override public void reset() { super.reset(); setStarted = setEnded = false; }
    @Override public void restrictDuration(long durationMillis) { super.restrictDuration(durationMillis); for (Animation a : animations) a.restrictDuration(durationMillis); }
    @Override public void scaleCurrentDuration(float scale) { for (Animation a : animations) a.scaleCurrentDuration(scale); }
    @Override public boolean willChangeTransformationMatrix() { for (Animation a : animations) if (a.willChangeTransformationMatrix()) return true; return false; }
    @Override public boolean willChangeBounds() { for (Animation a : animations) if (a.willChangeBounds()) return true; return false; }

    /** Gives the set's own settings to the children, then sizes them. */
    @Override public void initialize(int width, int height, int parentWidth, int parentHeight) {
        super.initialize(width, height, parentWidth, parentHeight);
        for (Animation a : animations) {
            if (durationSet) a.setDuration(duration);
            if (fillAfterSet) a.setFillAfter(getFillAfter());
            if (fillBeforeSet) a.setFillBefore(getFillBefore());
            if (repeatModeSet) a.setRepeatMode(getRepeatMode());
            if (shareInterpolator) a.setInterpolator(getInterpolator());
            if (startOffsetSet) a.setStartOffset(a.getStartOffset() + startOffset);
            a.initialize(width, height, parentWidth, parentHeight);
        }
    }

    @Override public boolean getTransformation(long currentTime, Transformation t) {
        boolean more = false, started = false, ended = true;
        t.clear();
        for (int i = animations.size() - 1; i >= 0; --i) {
            Animation a = animations.get(i);
            temp.clear();
            more |= a.getTransformation(currentTime, temp, getScaleFactor());
            t.compose(temp);
            started |= a.hasStarted();
            ended &= a.hasEnded();
        }
        if (started && !setStarted) {
            setStarted = true;
            if (setListener != null) setListener.onAnimationStart(this);
        }
        if (ended && !setEnded) {
            setEnded = true;
            if (setListener != null) setListener.onAnimationEnd(this);
        }
        return more;
    }
    @Override public boolean hasStarted() { return setStarted; }
    @Override public boolean hasEnded() { return setEnded; }
    @Override public void cancel() { for (Animation a : animations) a.cancel(); if (setStarted && !setEnded) { setEnded = true; if (setListener != null) setListener.onAnimationEnd(this); } }
    @Override protected AnimationSet clone() throws CloneNotSupportedException {
        AnimationSet s = (AnimationSet) super.clone();
        s.animations = new ArrayList<>();
        for (Animation a : animations) s.animations.add(a.clone());
        return s;
    }
}
