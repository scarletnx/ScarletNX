// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import java.util.ArrayList;

/** An animation of values over time. */
public abstract class Animator implements Cloneable {
    public static final long DURATION_INFINITE = -1;
    ArrayList<AnimatorListener> listeners;

    public interface AnimatorListener {
        default void onAnimationStart(Animator animation, boolean isReverse) { onAnimationStart(animation); }
        default void onAnimationEnd(Animator animation, boolean isReverse) { onAnimationEnd(animation); }
        void onAnimationStart(Animator animation);
        void onAnimationEnd(Animator animation);
        void onAnimationCancel(Animator animation);
        void onAnimationRepeat(Animator animation);
    }

    public interface AnimatorPauseListener {
        void onAnimationPause(Animator animation);
        void onAnimationResume(Animator animation);
    }

    public void start() {}
    public void cancel() {}
    public void end() {}
    public void pause() {}
    public void resume() {}
    public boolean isPaused() { return false; }
    public abstract long getStartDelay();
    public abstract void setStartDelay(long startDelay);
    public abstract Animator setDuration(long duration);
    public abstract long getDuration();
    public long getTotalDuration() { return getStartDelay() + getDuration(); }
    public abstract void setInterpolator(TimeInterpolator value);
    public TimeInterpolator getInterpolator() { return null; }
    public abstract boolean isRunning();
    public boolean isStarted() { return isRunning(); }
    public void addListener(AnimatorListener listener) {
        if (listeners == null) listeners = new ArrayList<>();
        listeners.add(listener);
    }
    public void removeListener(AnimatorListener listener) { if (listeners != null) listeners.remove(listener); }
    public ArrayList<AnimatorListener> getListeners() { return listeners; }
    public void addPauseListener(AnimatorPauseListener listener) {}
    public void removePauseListener(AnimatorPauseListener listener) {}
    public void removeAllListeners() { listeners = null; }
    public void setTarget(Object target) {}
    public void setupStartValues() {}
    public void setupEndValues() {}
    @Override public Animator clone() {
        try { return (Animator) super.clone(); } catch (CloneNotSupportedException e) { throw new AssertionError(e); }
    }
}
