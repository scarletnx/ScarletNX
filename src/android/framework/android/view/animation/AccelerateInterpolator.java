// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that starts slowly and speeds up. */
public class AccelerateInterpolator extends BaseInterpolator {
    private final float factor;

    public AccelerateInterpolator() { this(1); }
    public AccelerateInterpolator(float factor) { this.factor = factor; }
    public AccelerateInterpolator(Context context, AttributeSet attrs) {
        this(attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "factor", 1) : 1);
    }
    @Override public float getInterpolation(float input) { return factor == 1 ? input * input : (float) Math.pow(input, 2 * factor); }
}
