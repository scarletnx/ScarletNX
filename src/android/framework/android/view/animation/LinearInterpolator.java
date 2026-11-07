// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

/** Constant rate. */
public class LinearInterpolator extends BaseInterpolator {
    public LinearInterpolator() {}
    public LinearInterpolator(android.content.Context context, android.util.AttributeSet attrs) {}
    @Override public float getInterpolation(float input) { return input; }
}
