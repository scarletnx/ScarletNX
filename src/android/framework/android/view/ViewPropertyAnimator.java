// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** View property animations: the end values set at once. */
public class ViewPropertyAnimator {
    private final View view;
    private long duration = -1, startDelay;
    private android.animation.TimeInterpolator interpolator;

    ViewPropertyAnimator(View view) { this.view = view; }
    public ViewPropertyAnimator setDuration(long duration) {
        if (duration < 0) throw new IllegalArgumentException("Animators cannot have negative duration: " + duration);
        this.duration = duration;
        return this;
    }
    public long getDuration() { return duration >= 0 ? duration : 300; }
    public ViewPropertyAnimator setStartDelay(long startDelay) {
        if (startDelay < 0) throw new IllegalArgumentException("Animators cannot have negative start delay: " + startDelay);
        this.startDelay = startDelay;
        return this;
    }
    public long getStartDelay() { return startDelay; }
    public ViewPropertyAnimator setInterpolator(android.animation.TimeInterpolator interpolator) { this.interpolator = interpolator; return this; }
    public android.animation.TimeInterpolator getInterpolator() { return interpolator; }
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
    public ViewPropertyAnimator alphaBy(float value) { view.setAlpha(view.getAlpha() + value); return this; }
    public ViewPropertyAnimator translationXBy(float value) { view.setTranslationX(view.getTranslationX() + value); return this; }
    public ViewPropertyAnimator translationYBy(float value) { view.setTranslationY(view.getTranslationY() + value); return this; }
    public ViewPropertyAnimator translationZ(float value) { view.setTranslationZ(value); return this; }
    public ViewPropertyAnimator translationZBy(float value) { view.setTranslationZ(view.getTranslationZ() + value); return this; }
    public ViewPropertyAnimator scaleXBy(float value) { view.setScaleX(view.getScaleX() + value); return this; }
    public ViewPropertyAnimator scaleYBy(float value) { view.setScaleY(view.getScaleY() + value); return this; }
    public ViewPropertyAnimator rotationBy(float value) { view.setRotation(view.getRotation() + value); return this; }
    public ViewPropertyAnimator rotationX(float value) { view.setRotationX(value); return this; }
    public ViewPropertyAnimator rotationXBy(float value) { view.setRotationX(view.getRotationX() + value); return this; }
    public ViewPropertyAnimator rotationY(float value) { view.setRotationY(value); return this; }
    public ViewPropertyAnimator rotationYBy(float value) { view.setRotationY(view.getRotationY() + value); return this; }
    public ViewPropertyAnimator x(float value) { view.setX(value); return this; }
    public ViewPropertyAnimator xBy(float value) { view.setX(view.getX() + value); return this; }
    public ViewPropertyAnimator y(float value) { view.setY(value); return this; }
    public ViewPropertyAnimator yBy(float value) { view.setY(view.getY() + value); return this; }
    public ViewPropertyAnimator z(float value) { view.setZ(value); return this; }
    public ViewPropertyAnimator zBy(float value) { view.setZ(view.getZ() + value); return this; }
    public void start() {}
    public void cancel() {}
}
