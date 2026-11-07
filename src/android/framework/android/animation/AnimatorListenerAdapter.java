// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** An AnimatorListener that does nothing, to override some of. */
public abstract class AnimatorListenerAdapter implements Animator.AnimatorListener, Animator.AnimatorPauseListener {
    @Override public void onAnimationCancel(Animator animation) {}
    @Override public void onAnimationEnd(Animator animation) {}
    @Override public void onAnimationRepeat(Animator animation) {}
    @Override public void onAnimationStart(Animator animation) {}
    @Override public void onAnimationPause(Animator animation) {}
    @Override public void onAnimationResume(Animator animation) {}
}
