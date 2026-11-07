// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.graphics.Matrix;

/** The matrix and alpha that an animation applies at one time. */
public class Transformation {
    public static final int TYPE_IDENTITY = 0, TYPE_ALPHA = 1, TYPE_MATRIX = 2, TYPE_BOTH = 3;
    protected Matrix mMatrix = new Matrix();
    protected float mAlpha = 1;
    protected int mTransformationType = TYPE_BOTH;

    public Transformation() {}
    public void clear() { mMatrix.reset(); mAlpha = 1; mTransformationType = TYPE_BOTH; }
    public int getTransformationType() { return mTransformationType; }
    public void setTransformationType(int transformationType) { mTransformationType = transformationType; }
    public void set(Transformation t) { mAlpha = t.mAlpha; mMatrix.set(t.mMatrix); mTransformationType = t.mTransformationType; }
    public void compose(Transformation t) { mAlpha *= t.mAlpha; mMatrix.preConcat(t.mMatrix); }
    public void postCompose(Transformation t) { mAlpha *= t.mAlpha; mMatrix.postConcat(t.mMatrix); }
    public Matrix getMatrix() { return mMatrix; }
    public void setAlpha(float alpha) { mAlpha = alpha; }
    public float getAlpha() { return mAlpha; }
    @Override public String toString() { return "Transformation{alpha=" + mAlpha + " matrix=" + mMatrix + "}"; }
}
