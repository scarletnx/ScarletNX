// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that goes forward past the end and back to it. */
public class OvershootInterpolator extends BaseInterpolator {
    private final float tension;

    public OvershootInterpolator() { this(2); }
    public OvershootInterpolator(float tension) { this.tension = tension; }
    public OvershootInterpolator(Context context, AttributeSet attrs) {
        this(attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "tension", 2) : 2);
    }
    @Override public float getInterpolation(float t) { t -= 1; return t * t * ((tension + 1) * t + tension) + 1; }
}
