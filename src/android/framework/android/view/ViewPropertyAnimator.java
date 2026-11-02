// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** View property animations: the end values set at once. */
public class ViewPropertyAnimator {
    private final View view;

    ViewPropertyAnimator(View view) { this.view = view; }
    public ViewPropertyAnimator setDuration(long duration) { return this; }
    public ViewPropertyAnimator setStartDelay(long startDelay) { return this; }
    public ViewPropertyAnimator setInterpolator(android.animation.TimeInterpolator interpolator) { return this; }
    public ViewPropertyAnimator setListener(android.animation.Animator.AnimatorListener listener) { return this; }
    public ViewPropertyAnimator setUpdateListener(android.animation.ValueAnimator.AnimatorUpdateListener listener) { return this; }
    public ViewPropertyAnimator withEndAction(Runnable runnable) { view.post(runnable); return this; }
    public ViewPropertyAnimator withStartAction(Runnable runnable) { runnable.run(); return this; }
    public ViewPropertyAnimator withLayer() { return this; }
    public ViewPropertyAnimator alpha(float value) { view.setAlpha(value); return this; }
    public ViewPropertyAnimator translationX(float value) { view.setTranslationX(value); return this; }
    public ViewPropertyAnimator translationY(float value) { view.setTranslationY(value); return this; }
    public ViewPropertyAnimator scaleX(float value) { view.setScaleX(value); return this; }
    public ViewPropertyAnimator scaleY(float value) { view.setScaleY(value); return this; }
    public ViewPropertyAnimator rotation(float value) { view.setRotation(value); return this; }
    public void start() {}
    public void cancel() {}
}
