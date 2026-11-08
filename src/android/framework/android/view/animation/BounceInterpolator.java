// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that bounces at the end. */
public class BounceInterpolator extends BaseInterpolator {
    public BounceInterpolator() {}
    public BounceInterpolator(Context context, AttributeSet attrs) {}
    private static float bounce(float t) { return t * t * 8; }
    @Override public float getInterpolation(float t) {
        t *= 1.1226f;
        if (t < 0.3535f) return bounce(t);
        if (t < 0.7408f) return bounce(t - 0.54719f) + 0.7f;
        if (t < 0.9644f) return bounce(t - 0.8526f) + 0.9f;
        return bounce(t - 1.0435f) + 0.95f;
    }
}
