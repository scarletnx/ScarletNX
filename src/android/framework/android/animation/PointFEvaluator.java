// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import android.graphics.PointF;

/** Interpolates points, into a point of its own when it has one. */
public class PointFEvaluator implements TypeEvaluator<PointF> {
    private final PointF reuse;

    public PointFEvaluator() { reuse = null; }
    public PointFEvaluator(PointF reuse) { this.reuse = reuse; }
    @Override public PointF evaluate(float fraction, PointF startValue, PointF endValue) {
        float x = startValue.x + fraction * (endValue.x - startValue.x), y = startValue.y + fraction * (endValue.y - startValue.y);
        if (reuse == null) return new PointF(x, y);
        reuse.set(x, y);
        return reuse;
    }
}
