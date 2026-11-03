// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An insets animation the app controls. */
public interface WindowInsetsAnimationController {
    android.graphics.Insets getHiddenStateInsets();
    android.graphics.Insets getShownStateInsets();
    android.graphics.Insets getCurrentInsets();
    void setInsetsAndAlpha(android.graphics.Insets insets, float alpha, float fraction);
    void finish(boolean shown);
    float getCurrentFraction();
    float getCurrentAlpha();
    int getTypes();
    boolean isFinished();
    boolean isCancelled();
    default boolean isReady() { return !isFinished() && !isCancelled(); }
}
