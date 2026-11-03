// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** A rate that first goes back, then forward. */
public class AnticipateInterpolator extends BaseInterpolator {
    private final float tension;

    public AnticipateInterpolator() { this(2); }
    public AnticipateInterpolator(float tension) { this.tension = tension; }
    public AnticipateInterpolator(Context context, AttributeSet attrs) {
        this(attrs != null ? attrs.getAttributeFloatValue(Animation.ANDROID, "tension", 2) : 2);
    }
    @Override public float getInterpolation(float t) { return t * t * ((tension + 1) * t - tension); }
}
