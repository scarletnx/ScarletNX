// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** An animation of the scale of a view around a pivot. */
public class ScaleAnimation extends Animation {
    private final float fromX, toX, fromY, toY;
    private int pivotXType = ABSOLUTE, pivotYType = ABSOLUTE;
    private float pivotXValue, pivotYValue, pivotX, pivotY;

    public ScaleAnimation(Context context, AttributeSet attrs) {
        super(context, attrs);
        fromX = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "fromXScale", 0) : 0;
        toX = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "toXScale", 0) : 0;
        fromY = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "fromYScale", 0) : 0;
        toY = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "toYScale", 0) : 0;
        float[] x = AnimationUtils.size(attrs, "pivotX"), y = AnimationUtils.size(attrs, "pivotY");
        pivotXType = (int) x[0];
        pivotXValue = x[1];
        pivotYType = (int) y[0];
        pivotYValue = y[1];
    }
    public ScaleAnimation(float fromX, float toX, float fromY, float toY) { this.fromX = fromX; this.toX = toX; this.fromY = fromY; this.toY = toY; }
    public ScaleAnimation(float fromX, float toX, float fromY, float toY, float pivotX, float pivotY) {
        this(fromX, toX, fromY, toY);
        pivotXValue = this.pivotX = pivotX;
        pivotYValue = this.pivotY = pivotY;
    }
    public ScaleAnimation(float fromX, float toX, float fromY, float toY, int pivotXType, float pivotXValue, int pivotYType, float pivotYValue) {
        this(fromX, toX, fromY, toY);
        this.pivotXType = pivotXType;
        this.pivotXValue = pivotXValue;
        this.pivotYType = pivotYType;
        this.pivotYValue = pivotYValue;
        if (pivotXType == ABSOLUTE) pivotX = pivotXValue;
        if (pivotYType == ABSOLUTE) pivotY = pivotYValue;
    }
    @Override public void initialize(int width, int height, int parentWidth, int parentHeight) {
        super.initialize(width, height, parentWidth, parentHeight);
        pivotX = resolveSize(pivotXType, pivotXValue, width, parentWidth);
        pivotY = resolveSize(pivotYType, pivotYValue, height, parentHeight);
    }
    @Override protected void applyTransformation(float interpolatedTime, Transformation t) {
        float sx = fromX + (toX - fromX) * interpolatedTime, sy = fromY + (toY - fromY) * interpolatedTime;
        if (pivotX == 0 && pivotY == 0) t.getMatrix().setScale(sx, sy);
        else t.getMatrix().setScale(sx, sy, pivotX, pivotY);
    }
}
