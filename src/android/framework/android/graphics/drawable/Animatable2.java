// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

/** An animatable drawable that tells callbacks when its animation starts and ends. */
public interface Animatable2 extends Animatable {
    void registerAnimationCallback(AnimationCallback callback);

    boolean unregisterAnimationCallback(AnimationCallback callback);

    void clearAnimationCallbacks();

    abstract class AnimationCallback {
        public AnimationCallback() {}
        public void onAnimationStart(Drawable drawable) {}
        public void onAnimationEnd(Drawable drawable) {}
    }
}
