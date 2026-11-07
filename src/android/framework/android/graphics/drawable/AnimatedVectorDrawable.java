// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;
import java.util.ArrayList;

/**
 * A vector drawable with animators. It draws nothing (as VectorDrawable here), and its animation has no frames: a
 * start tells the callbacks that the animation started, and a stop that it ended.
 */
public class AnimatedVectorDrawable extends Drawable implements Animatable2 {
    private final ArrayList<AnimationCallback> callbacks = new ArrayList<>();
    private boolean running;
    private int alpha = 0xff;
    private ColorFilter filter;

    public AnimatedVectorDrawable() {}
    @Override public void draw(Canvas canvas) {}
    @Override public void setAlpha(int alpha) { this.alpha = alpha; }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }

    @Override public void start() {
        if (running) return;
        running = true;
        for (AnimationCallback c : new ArrayList<>(callbacks)) c.onAnimationStart(this);
    }
    @Override public void stop() {
        if (!running) return;
        running = false;
        for (AnimationCallback c : new ArrayList<>(callbacks)) c.onAnimationEnd(this);
    }
    public void reset() { running = false; }
    public void reverse() {}
    public boolean canReverse() { return false; }
    @Override public boolean isRunning() { return running; }
    @Override public void registerAnimationCallback(AnimationCallback callback) { if (callback != null && !callbacks.contains(callback)) callbacks.add(callback); }
    @Override public boolean unregisterAnimationCallback(AnimationCallback callback) { return callbacks.remove(callback); }
    @Override public void clearAnimationCallbacks() { callbacks.clear(); }
}
