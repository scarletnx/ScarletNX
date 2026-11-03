// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** An animation of the alpha of a view. */
public class AlphaAnimation extends Animation {
    private final float from, to;

    public AlphaAnimation(Context context, AttributeSet attrs) {
        super(context, attrs);
        from = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "fromAlpha", 1) : 1;
        to = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "toAlpha", 1) : 1;
    }
    public AlphaAnimation(float fromAlpha, float toAlpha) { from = fromAlpha; to = toAlpha; }
    @Override protected void applyTransformation(float interpolatedTime, Transformation t) { t.setAlpha(from + (to - from) * interpolatedTime); }
    @Override public boolean willChangeTransformationMatrix() { return false; }
    @Override public boolean willChangeBounds() { return false; }
}
