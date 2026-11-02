// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

/** A rate that starts fast and slows down. */
public class DecelerateInterpolator extends BaseInterpolator {
    private final float factor;
    public DecelerateInterpolator() { this(1); }
    public DecelerateInterpolator(float factor) { this.factor = factor; }
    @Override public float getInterpolation(float input) {
        return factor == 1 ? 1 - (1 - input) * (1 - input) : (float) (1 - Math.pow(1 - input, 2 * factor));
    }
}
