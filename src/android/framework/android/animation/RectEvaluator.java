// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import android.graphics.Rect;

/** Interpolates rectangles, into a rectangle of its own when it has one. */
public class RectEvaluator implements TypeEvaluator<Rect> {
    private final Rect reuse;

    public RectEvaluator() { reuse = null; }
    public RectEvaluator(Rect reuse) { this.reuse = reuse; }
    @Override public Rect evaluate(float fraction, Rect startValue, Rect endValue) {
        int l = startValue.left + (int) ((endValue.left - startValue.left) * fraction);
        int t = startValue.top + (int) ((endValue.top - startValue.top) * fraction);
        int r = startValue.right + (int) ((endValue.right - startValue.right) * fraction);
        int b = startValue.bottom + (int) ((endValue.bottom - startValue.bottom) * fraction);
        if (reuse == null) return new Rect(l, t, r, b);
        reuse.set(l, t, r, b);
        return reuse;
    }
}
