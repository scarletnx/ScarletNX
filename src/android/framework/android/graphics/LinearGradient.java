// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A gradient along a line. */
public class LinearGradient extends Shader {
    public LinearGradient(float x0, float y0, float x1, float y1, int[] colors, float[] positions, TileMode tile) {
        kind = 1;
        points = new float[] {x0, y0, x1, y1};
        this.colors = colors.clone();
        this.positions = positions != null ? positions.clone() : null;
        this.tile = tile;
    }

    public LinearGradient(float x0, float y0, float x1, float y1, long[] colors, float[] positions, TileMode tile) {
        this(x0, y0, x1, y1, argb(colors), positions, tile);
    }

    public LinearGradient(float x0, float y0, float x1, float y1, int color0, int color1, TileMode tile) {
        this(x0, y0, x1, y1, new int[] {color0, color1}, null, tile);
    }

    public LinearGradient(float x0, float y0, float x1, float y1, long color0, long color1, TileMode tile) {
        this(x0, y0, x1, y1, new int[] {Color.toArgb(color0), Color.toArgb(color1)}, null, tile);
    }
}
