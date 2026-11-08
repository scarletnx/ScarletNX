// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that first goes back, then forward past the end and back to it. */
public class AnticipateOvershootInterpolator extends BaseInterpolator {
    private final float tension;

    public AnticipateOvershootInterpolator() { this(2, 1.5f); }
    public AnticipateOvershootInterpolator(float tension) { this(tension, 1.5f); }
    public AnticipateOvershootInterpolator(float tension, float extraTension) { this.tension = tension * extraTension; }
    public AnticipateOvershootInterpolator(Context context, AttributeSet attrs) {
        this(attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "tension", 2) : 2,
            attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "extraTension", 1.5f) : 1.5f);
    }
    @Override public float getInterpolation(float t) {
        if (t < 0.5f) { float u = t * 2; return 0.5f * u * u * ((tension + 1) * u - tension); }
        float u = t * 2 - 2;
        return 0.5f * (u * u * ((tension + 1) * u + tension) + 2);
    }
}
