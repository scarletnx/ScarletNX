// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A gradient around a center. */
public class SweepGradient extends Shader {
    public SweepGradient(float cx, float cy, int[] colors, float[] positions) {
        kind = 3;
        points = new float[] {cx, cy};
        this.colors = colors.clone();
        this.positions = positions != null ? positions.clone() : null;
    }

    public SweepGradient(float cx, float cy, long[] colors, float[] positions) { this(cx, cy, argb(colors), positions); }
    public SweepGradient(float cx, float cy, int color0, int color1) { this(cx, cy, new int[] {color0, color1}, null); }
    public SweepGradient(float cx, float cy, long color0, long color1) {
        this(cx, cy, new int[] {Color.toArgb(color0), Color.toArgb(color1)}, null);
    }
}
