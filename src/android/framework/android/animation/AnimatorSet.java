// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.animation;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/** Animators played together or one after another. */
public final class AnimatorSet extends Animator {
    private ArrayList<Animator> nodes = new ArrayList<>();
    /** For each node, the node it starts after, or null to start with the set. */
    private ArrayList<Animator> after = new ArrayList<>();
    private ArrayList<Animator> done = new ArrayList<>();
    private long duration = -1, startDelay;
    private TimeInterpolator interpolator;
    private boolean started;
    private AnimatorListenerAdapter childListener = new AnimatorListenerAdapter() {
        @Override public void onAnimationEnd(Animator animation) { childEnded(animation); }
    };

    public AnimatorSet() {}

    /** Gives the order of a set's animators, relative to one animator. */
    public class Builder {
        private final Animator anchor;

        Builder(Animator anchor) { this.anchor = anchor; add(anchor, null); }
        public Builder with(Animator anim) { add(anim, after.get(nodes.indexOf(anchor))); return this; }
        public Builder before(Animator anim) { add(anim, anchor); return this; }
        public Builder after(Animator anim) {
            add(anim, null);
            after.set(nodes.indexOf(anchor), anim);
            return this;
        }
        public Builder after(long delay) {
            ValueAnimator wait = ValueAnimator.ofFloat(0, 1);
            wait.setDuration(delay);
            return after(wait);
        }
    }

    private void add(Animator a, Animator dependency) {
        int i = nodes.indexOf(a);
        if (i < 0) { nodes.add(a); after.add(dependency); }
        else if (dependency != null) after.set(i, dependency);
    }
    public Builder play(Animator anim) { return anim != null ? new Builder(anim) : null; }
    public void playTogether(Animator... items) { for (Animator a : items) add(a, null); }
    public void playTogether(Collection<Animator> items) { for (Animator a : items) add(a, null); }
    public void playSequentially(Animator... items) {
        for (int i = 0; i < items.length; ++i) add(items[i], i > 0 ? items[i - 1] : null);
    }
    public void playSequentially(List<Animator> items) { playSequentially(items.toArray(new Animator[0])); }
    public ArrayList<Animator> getChildAnimations() { return new ArrayList<>(nodes); }

    @Override public AnimatorSet setDuration(long duration) {
        if (duration < 0) throw new IllegalArgumentException("duration must be a value of zero or greater");
        this.duration = duration;
        return this;
    }
    @Override public long getDuration() { return duration; }
    @Override public long getStartDelay() { return startDelay; }
    @Override public void setStartDelay(long startDelay) { this.startDelay = startDelay; }
    @Override public void setInterpolator(TimeInterpolator interpolator) { this.interpolator = interpolator; }
    @Override public TimeInterpolator getInterpolator() { return interpolator; }
    @Override public void setTarget(Object target) { for (Animator a : nodes) a.setTarget(target); }
    @Override public boolean isStarted() { return started; }
    @Override public boolean isRunning() {
        for (Animator a : nodes) if (a.isRunning()) return true;
        return false;
    }

    @Override public void start() {
        if (started) return;
        started = true;
        done.clear();
        for (Animator a : nodes) {
            if (duration >= 0) a.setDuration(duration);
            if (interpolator != null) a.setInterpolator(interpolator);
            a.removeListener(childListener);
            a.addListener(childListener);
        }
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) l.onAnimationStart(this);
        if (startDelay > 0) new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::startRoots, startDelay);
        else startRoots();
    }
    private void startRoots() {
        if (!started) return;
        for (int i = 0; i < nodes.size(); ++i) if (after.get(i) == null) nodes.get(i).start();
        if (nodes.isEmpty()) finish();
    }
    private void childEnded(Animator a) {
        if (!started) return;
        if (!done.contains(a)) done.add(a);
        for (int i = 0; i < nodes.size(); ++i) if (after.get(i) == a && !done.contains(nodes.get(i))) nodes.get(i).start();
        if (done.size() == nodes.size()) finish();
    }
    private void finish() {
        started = false;
        for (Animator a : nodes) a.removeListener(childListener);
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) l.onAnimationEnd(this);
    }
    @Override public void cancel() {
        if (!started) return;
        started = false;
        for (Animator a : nodes) if (a.isRunning()) a.cancel();
        for (Animator a : nodes) a.removeListener(childListener);
        if (listeners != null) for (AnimatorListener l : new ArrayList<>(listeners)) { l.onAnimationCancel(this); l.onAnimationEnd(this); }
    }
    @Override public void end() {
        if (!started) start();
        for (Animator a : new ArrayList<>(nodes)) {
            if (!started) break;
            if (done.contains(a)) continue;
            if (!a.isRunning()) a.start();
            a.end();
            if (!done.contains(a)) childEnded(a);
        }
    }
    @Override public AnimatorSet clone() {
        AnimatorSet s = (AnimatorSet) super.clone();
        s.nodes = new ArrayList<>();
        s.after = new ArrayList<>();
        s.done = new ArrayList<>();
        s.started = false;
        if (listeners != null) s.listeners = new ArrayList<>(listeners);
        s.childListener = new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) { s.childEnded(animation); }
        };
        for (Animator a : nodes) { Animator c = a.clone(); c.removeListener(childListener); s.nodes.add(c); }
        for (Animator d : after) s.after.add(d != null ? s.nodes.get(nodes.indexOf(d)) : null);
        return s;
    }
}
