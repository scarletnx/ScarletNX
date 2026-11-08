// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.content.Context;
import android.util.AttributeSet;

/** An animation of the rotation of a view around a pivot. */
public class RotateAnimation extends Animation {
    private final float fromDegrees, toDegrees;
    private int pivotXType = ABSOLUTE, pivotYType = ABSOLUTE;
    private float pivotXValue, pivotYValue, pivotX, pivotY;

    public RotateAnimation(Context context, AttributeSet attrs) {
        super(context, attrs);
        fromDegrees = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "fromDegrees", 0) : 0;
        toDegrees = attrs != null ? attrs.getAttributeFloatValue(ANDROID, "toDegrees", 0) : 0;
        float[] x = AnimationUtils.size(attrs, "pivotX"), y = AnimationUtils.size(attrs, "pivotY");
        pivotXType = (int) x[0];
        pivotXValue = x[1];
        pivotYType = (int) y[0];
        pivotYValue = y[1];
    }
    public RotateAnimation(float fromDegrees, float toDegrees) { this.fromDegrees = fromDegrees; this.toDegrees = toDegrees; }
    public RotateAnimation(float fromDegrees, float toDegrees, float pivotX, float pivotY) {
        this(fromDegrees, toDegrees);
        pivotXValue = this.pivotX = pivotX;
        pivotYValue = this.pivotY = pivotY;
    }
    public RotateAnimation(float fromDegrees, float toDegrees, int pivotXType, float pivotXValue, int pivotYType, float pivotYValue) {
        this(fromDegrees, toDegrees);
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
        float degrees = fromDegrees + (toDegrees - fromDegrees) * interpolatedTime;
        if (pivotX == 0 && pivotY == 0) t.getMatrix().setRotate(degrees);
        else t.getMatrix().setRotate(degrees, pivotX, pivotY);
    }
}
