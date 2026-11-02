// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A gradient out from a center. */
public class RadialGradient extends Shader {
    public RadialGradient(float centerX, float centerY, float radius, int[] colors, float[] stops, TileMode tile) {
        kind = 2;
        points = new float[] {centerX, centerY, radius};
        this.colors = colors.clone();
        positions = stops != null ? stops.clone() : null;
        this.tile = tile;
    }

    public RadialGradient(float centerX, float centerY, float radius, long[] colors, float[] stops, TileMode tile) {
        this(centerX, centerY, radius, argb(colors), stops, tile);
    }

    public RadialGradient(float centerX, float centerY, float radius, int centerColor, int edgeColor, TileMode tile) {
        this(centerX, centerY, radius, new int[] {centerColor, edgeColor}, null, tile);
    }

    public RadialGradient(float centerX, float centerY, float radius, long centerColor, long edgeColor, TileMode tile) {
        this(centerX, centerY, radius, new int[] {Color.toArgb(centerColor), Color.toArgb(edgeColor)}, null, tile);
    }
}
