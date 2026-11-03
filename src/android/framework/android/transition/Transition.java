// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

import android.animation.TimeInterpolator;
import android.graphics.Rect;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

/** A change of a scene, animated. Here a scene changes at once: a transition only tells its listeners. */
public abstract class Transition implements Cloneable {
    private long duration = -1, startDelay = -1;
    private TimeInterpolator interpolator;
    private String name = getClass().getName();
    private ArrayList<Integer> targetIds = new ArrayList<>();
    private ArrayList<View> targets = new ArrayList<>();
    private ArrayList<String> targetNames = new ArrayList<>();
    private ArrayList<Class<?>> targetTypes = new ArrayList<>();
    ArrayList<TransitionListener> listeners = new ArrayList<>();
    private EpicenterCallback epicenterCallback;

    public interface TransitionListener {
        void onTransitionStart(Transition transition);
        void onTransitionEnd(Transition transition);
        void onTransitionCancel(Transition transition);
        void onTransitionPause(Transition transition);
        void onTransitionResume(Transition transition);
        default void onTransitionStart(Transition transition, boolean isReverse) { onTransitionStart(transition); }
        default void onTransitionEnd(Transition transition, boolean isReverse) { onTransitionEnd(transition); }
    }

    public abstract static class EpicenterCallback {
        public EpicenterCallback() {}
        public abstract Rect onGetEpicenter(Transition transition);
    }

    public Transition() {}
    public Transition(android.content.Context context, android.util.AttributeSet attrs) {}

    public Transition setDuration(long duration) { this.duration = duration; return this; }
    public long getDuration() { return duration; }
    public Transition setStartDelay(long startDelay) { this.startDelay = startDelay; return this; }
    public long getStartDelay() { return startDelay; }
    public Transition setInterpolator(TimeInterpolator interpolator) { this.interpolator = interpolator; return this; }
    public TimeInterpolator getInterpolator() { return interpolator; }
    public String getName() { return name; }
    public String[] getTransitionProperties() { return null; }

    public Transition addTarget(int targetId) { if (targetId != 0) targetIds.add(targetId); return this; }
    public Transition addTarget(String targetName) { targetNames.add(targetName); return this; }
    public Transition addTarget(Class targetType) { targetTypes.add(targetType); return this; }
    public Transition addTarget(View target) { targets.add(target); return this; }
    public Transition removeTarget(int targetId) { targetIds.remove((Integer) targetId); return this; }
    public Transition removeTarget(String targetName) { targetNames.remove(targetName); return this; }
    public Transition removeTarget(Class target) { targetTypes.remove(target); return this; }
    public Transition removeTarget(View target) { targets.remove(target); return this; }
    public Transition excludeTarget(int targetId, boolean exclude) { return this; }
    public Transition excludeTarget(String targetName, boolean exclude) { return this; }
    public Transition excludeTarget(View target, boolean exclude) { return this; }
    public Transition excludeTarget(Class type, boolean exclude) { return this; }
    public Transition excludeChildren(int targetId, boolean exclude) { return this; }
    public Transition excludeChildren(View target, boolean exclude) { return this; }
    public Transition excludeChildren(Class type, boolean exclude) { return this; }
    public List<Integer> getTargetIds() { return targetIds; }
    public List<View> getTargets() { return targets; }
    public List<String> getTargetNames() { return targetNames; }
    public List<Class> getTargetTypes() { return new ArrayList<>(targetTypes); }

    public Transition addListener(TransitionListener listener) { listeners.add(listener); return this; }
    public Transition removeListener(TransitionListener listener) { listeners.remove(listener); return this; }
    public void setEpicenterCallback(EpicenterCallback epicenterCallback) { this.epicenterCallback = epicenterCallback; }
    public EpicenterCallback getEpicenterCallback() { return epicenterCallback; }
    public Rect getEpicenter() { return epicenterCallback != null ? epicenterCallback.onGetEpicenter(this) : null; }

    /** The transition ran and ended at once: its listeners see a start and an end. */
    void runAtOnce() {
        for (TransitionListener l : new ArrayList<>(listeners)) l.onTransitionStart(this, false);
        for (TransitionListener l : new ArrayList<>(listeners)) l.onTransitionEnd(this, false);
    }

    @Override public Transition clone() {
        try {
            Transition t = (Transition) super.clone();
            t.targetIds = new ArrayList<>(targetIds);
            t.targets = new ArrayList<>(targets);
            t.targetNames = new ArrayList<>(targetNames);
            t.targetTypes = new ArrayList<>(targetTypes);
            t.listeners = new ArrayList<>(listeners);
            return t;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }
    @Override public String toString() { return getClass().getSimpleName() + "@" + Integer.toHexString(hashCode()); }
}
