// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** What a draw fills with in place of the paint's color: a gradient or a bitmap. */
public class Shader {
    public enum TileMode { CLAMP, REPEAT, MIRROR, DECAL }

    /** 0 none, 1 linear, 2 radial, 3 sweep, 4 bitmap. */
    int kind;
    float[] points = new float[0];
    int[] colors = new int[0];
    float[] positions;
    TileMode tile = TileMode.CLAMP, tileY = TileMode.CLAMP;
    Bitmap bitmap;
    private final Matrix local = new Matrix();

    public Shader() {}

    public boolean getLocalMatrix(Matrix out) { out.set(local); return !local.isIdentity(); }
    public void setLocalMatrix(Matrix m) { local.set(m); }
    Matrix localMatrix() { return local; }

    static int[] argb(long[] colors) {
        int[] out = new int[colors.length];
        for (int i = 0; i < colors.length; ++i) out[i] = Color.toArgb(colors[i]);
        return out;
    }
}
