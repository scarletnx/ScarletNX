// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A 4x5 matrix applied to each pixel's color. */
public class ColorMatrixColorFilter extends ColorFilter {
    public ColorMatrixColorFilter(float[] array) {
        kind = 2;
        matrix = array.clone();
    }

    public ColorMatrixColorFilter(ColorMatrix m) { this(m.getArray()); }

    public void getColorMatrix(ColorMatrix out) { out.set(matrix); }
}
