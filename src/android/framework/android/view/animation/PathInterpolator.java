// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

import android.graphics.Path;

/** A rate along a cubic curve from (0, 0) to (1, 1). */
public class PathInterpolator extends BaseInterpolator {
    private final float x1, y1, x2, y2;
    public PathInterpolator(float controlX, float controlY) { this(controlX * 2 / 3, controlY * 2 / 3, 1 - (1 - controlX) * 2 / 3, 1 - (1 - controlY) * 2 / 3); }
    public PathInterpolator(float controlX1, float controlY1, float controlX2, float controlY2) { x1 = controlX1; y1 = controlY1; x2 = controlX2; y2 = controlY2; }
    public PathInterpolator(Path path) { this(0.4f, 0, 0.2f, 1); }
    private static float bez(float t, float a, float b) { float u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; }
    @Override public float getInterpolation(float t) {
        if (t <= 0) return 0;
        if (t >= 1) return 1;
        float lo = 0, hi = 1, s = t;
        for (int i = 0; i < 30; ++i) {
            s = (lo + hi) / 2;
            if (bez(s, x1, x2) < t) lo = s; else hi = s;
        }
        return bez(s, y1, y2);
    }
}
