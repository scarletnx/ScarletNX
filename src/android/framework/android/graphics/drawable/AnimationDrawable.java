// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.Resources;
import android.os.SystemClock;
import java.util.Arrays;

/** Frames shown one after another, each for its duration. */
public class AnimationDrawable extends DrawableContainer implements Runnable, Animatable {
    private final AnimationState state;
    private int frame = -1;
    private boolean running, animating;

    public AnimationDrawable() { this(null, null); }
    private AnimationDrawable(AnimationState orig, Resources res) {
        state = new AnimationState(orig, this, res);
        setConstantState(state);
        if (orig != null) setFrame(0, true, false);
    }

    static final class AnimationState extends DrawableContainerState {
        int[] durations;
        boolean oneShot;

        AnimationState(AnimationState orig, AnimationDrawable owner, Resources res) {
            super(orig, owner, res);
            durations = orig != null ? orig.durations.clone() : new int[getCapacity()];
            oneShot = orig != null && orig.oneShot;
        }
        @Override public Drawable newDrawable() { return new AnimationDrawable(this, null); }
        @Override public Drawable newDrawable(Resources res) { return new AnimationDrawable(this, res); }
    }

    public void addFrame(Drawable frame, int duration) {
        int pos = state.addChild(frame);
        if (pos >= state.durations.length) state.durations = Arrays.copyOf(state.durations, state.getCapacity());
        state.durations[pos] = duration;
        if (this.frame < 0) setFrame(0, true, false);
    }
    public int getNumberOfFrames() { return state.getChildCount(); }
    public Drawable getFrame(int index) { return state.getChild(index); }
    public int getDuration(int i) { return state.durations[i]; }
    public boolean isOneShot() { return state.oneShot; }
    public void setOneShot(boolean oneShot) { state.oneShot = oneShot; }

    @Override public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = super.setVisible(visible, restart);
        if (visible) {
            if (restart || changed) setFrame(restart || (!running && !state.oneShot) || frame >= getNumberOfFrames() ? 0 : frame, true, animating);
        } else {
            unscheduleSelf(this);
        }
        return changed;
    }
    @Override public void start() {
        animating = true;
        if (!running) setFrame(0, false, getNumberOfFrames() > 1 || !state.oneShot);
    }
    @Override public void stop() {
        animating = false;
        if (running) {
            frame = -1;
            running = false;
            unscheduleSelf(this);
        }
    }
    @Override public boolean isRunning() { return running; }
    @Override public void run() {
        int next = frame + 1, n = getNumberOfFrames();
        boolean last = state.oneShot && next >= n - 1;
        if (!state.oneShot && next >= n) next = 0;
        setFrame(next, false, !last);
    }
    private void setFrame(int f, boolean unschedule, boolean animate) {
        if (f >= getNumberOfFrames()) return;
        animating = animate;
        frame = f;
        selectDrawable(f);
        if (unschedule || animate) unscheduleSelf(this);
        running = animate;
        if (animate) scheduleSelf(this, SystemClock.uptimeMillis() + state.durations[f]);
    }
}
