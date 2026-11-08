// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** An animation of the position of a view. */
public class TranslateAnimation extends Animation {
    private int fromXType = ABSOLUTE, toXType = ABSOLUTE, fromYType = ABSOLUTE, toYType = ABSOLUTE;
    private float fromXValue, toXValue, fromYValue, toYValue, fromX, toX, fromY, toY;

    public TranslateAnimation(Context context, AttributeSet attrs) {
        super(context, attrs);
        float[] a = AnimationUtils.size(attrs, "fromXDelta"), b = AnimationUtils.size(attrs, "toXDelta");
        float[] c = AnimationUtils.size(attrs, "fromYDelta"), d = AnimationUtils.size(attrs, "toYDelta");
        fromXType = (int) a[0]; fromXValue = a[1];
        toXType = (int) b[0]; toXValue = b[1];
        fromYType = (int) c[0]; fromYValue = c[1];
        toYType = (int) d[0]; toYValue = d[1];
    }
    public TranslateAnimation(float fromXDelta, float toXDelta, float fromYDelta, float toYDelta) {
        fromXValue = fromX = fromXDelta;
        toXValue = toX = toXDelta;
        fromYValue = fromY = fromYDelta;
        toYValue = toY = toYDelta;
    }
    public TranslateAnimation(int fromXType, float fromXValue, int toXType, float toXValue, int fromYType, float fromYValue, int toYType, float toYValue) {
        this.fromXType = fromXType; this.fromXValue = fromXValue;
        this.toXType = toXType; this.toXValue = toXValue;
        this.fromYType = fromYType; this.fromYValue = fromYValue;
        this.toYType = toYType; this.toYValue = toYValue;
    }
    @Override public void initialize(int width, int height, int parentWidth, int parentHeight) {
        super.initialize(width, height, parentWidth, parentHeight);
        fromX = resolveSize(fromXType, fromXValue, width, parentWidth);
        toX = resolveSize(toXType, toXValue, width, parentWidth);
        fromY = resolveSize(fromYType, fromYValue, height, parentHeight);
        toY = resolveSize(toYType, toYValue, height, parentHeight);
    }
    @Override protected void applyTransformation(float interpolatedTime, Transformation t) {
        t.getMatrix().setTranslate(fromX + (toX - fromX) * interpolatedTime, fromY + (toY - fromY) * interpolatedTime);
    }
    @Override public boolean willChangeBounds() { return false; }
}
