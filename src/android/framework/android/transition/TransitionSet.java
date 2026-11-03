// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

import android.animation.TimeInterpolator;
import android.view.View;
import java.util.ArrayList;

/** Transitions played together or one after another. */
public class TransitionSet extends Transition {
    public static final int ORDERING_TOGETHER = 0, ORDERING_SEQUENTIAL = 1;
    private ArrayList<Transition> transitions = new ArrayList<>();
    private int ordering = ORDERING_TOGETHER;

    public TransitionSet() {}
    public TransitionSet(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); }

    public TransitionSet setOrdering(int ordering) {
        if (ordering != ORDERING_TOGETHER && ordering != ORDERING_SEQUENTIAL)
            throw new android.util.AndroidRuntimeException("Invalid parameter for TransitionSet ordering: " + ordering);
        this.ordering = ordering;
        return this;
    }
    public int getOrdering() { return ordering; }
    public TransitionSet addTransition(Transition transition) {
        if (transition != null) {
            transitions.add(transition);
            if (getDuration() >= 0) transition.setDuration(getDuration());
            if (getInterpolator() != null) transition.setInterpolator(getInterpolator());
        }
        return this;
    }
    public TransitionSet removeTransition(Transition transition) { transitions.remove(transition); return this; }
    public int getTransitionCount() { return transitions.size(); }
    public Transition getTransitionAt(int index) { return index >= 0 && index < transitions.size() ? transitions.get(index) : null; }

    @Override public TransitionSet setDuration(long duration) {
        super.setDuration(duration);
        if (duration >= 0) for (Transition t : transitions) t.setDuration(duration);
        return this;
    }
    @Override public TransitionSet setStartDelay(long startDelay) { super.setStartDelay(startDelay); return this; }
    @Override public TransitionSet setInterpolator(TimeInterpolator interpolator) {
        super.setInterpolator(interpolator);
        for (Transition t : transitions) t.setInterpolator(interpolator);
        return this;
    }
    @Override public TransitionSet addTarget(View target) { for (Transition t : transitions) t.addTarget(target); super.addTarget(target); return this; }
    @Override public TransitionSet addTarget(int targetId) { for (Transition t : transitions) t.addTarget(targetId); super.addTarget(targetId); return this; }
    @Override public TransitionSet addTarget(String targetName) { for (Transition t : transitions) t.addTarget(targetName); super.addTarget(targetName); return this; }
    @Override public TransitionSet addTarget(Class targetType) { for (Transition t : transitions) t.addTarget(targetType); super.addTarget(targetType); return this; }
    @Override public TransitionSet removeTarget(View target) { for (Transition t : transitions) t.removeTarget(target); super.removeTarget(target); return this; }
    @Override public TransitionSet removeTarget(int targetId) { for (Transition t : transitions) t.removeTarget(targetId); super.removeTarget(targetId); return this; }
    @Override public TransitionSet removeTarget(String targetName) { for (Transition t : transitions) t.removeTarget(targetName); super.removeTarget(targetName); return this; }
    @Override public TransitionSet removeTarget(Class target) { for (Transition t : transitions) t.removeTarget(target); super.removeTarget(target); return this; }
    @Override public TransitionSet addListener(TransitionListener listener) { super.addListener(listener); return this; }
    @Override public TransitionSet removeListener(TransitionListener listener) { super.removeListener(listener); return this; }

    @Override void runAtOnce() {
        for (TransitionListener l : new ArrayList<>(listeners)) l.onTransitionStart(this, false);
        for (Transition t : transitions) t.runAtOnce();
        for (TransitionListener l : new ArrayList<>(listeners)) l.onTransitionEnd(this, false);
    }
    @Override public TransitionSet clone() {
        TransitionSet s = (TransitionSet) super.clone();
        s.transitions = new ArrayList<>();
        for (Transition t : transitions) s.transitions.add(t.clone());
        return s;
    }
}
