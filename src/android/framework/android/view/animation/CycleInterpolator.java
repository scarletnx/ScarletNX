// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that repeats a sine wave a number of cycles. */
public class CycleInterpolator extends BaseInterpolator {
    private final float cycles;

    public CycleInterpolator(float cycles) { this.cycles = cycles; }
    public CycleInterpolator(Context context, AttributeSet attrs) {
        this(attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "cycles", 1) : 1);
    }
    @Override public float getInterpolation(float input) { return (float) Math.sin(2 * cycles * Math.PI * input); }
}
