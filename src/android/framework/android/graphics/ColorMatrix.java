// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A 4x5 color matrix. */
public class ColorMatrix {
    private final float[] array = new float[20];

    public ColorMatrix() { reset(); }
    public ColorMatrix(float[] src) { set(src); }
    public ColorMatrix(ColorMatrix src) { set(src.array); }

    public final float[] getArray() { return array; }
    public void reset() { java.util.Arrays.fill(array, 0); array[0] = array[6] = array[12] = array[18] = 1; }
    public void set(ColorMatrix src) { set(src.array); }
    public void set(float[] src) { System.arraycopy(src, 0, array, 0, 20); }
    public void setScale(float r, float g, float b, float a) { java.util.Arrays.fill(array, 0); array[0] = r; array[6] = g; array[12] = b; array[18] = a; }
    public void setSaturation(float sat) {
        reset();
        float i = 1 - sat, r = 0.213f * i, g = 0.715f * i, b = 0.072f * i;
        array[0] = r + sat; array[1] = g; array[2] = b;
        array[5] = r; array[6] = g + sat; array[7] = b;
        array[10] = r; array[11] = g; array[12] = b + sat;
    }
    public void setConcat(ColorMatrix a, ColorMatrix b) {
        float[] t = new float[20];
        for (int j = 0; j < 20; j += 5)
            for (int i = 0; i < 4; ++i)
                t[j + i] = a.array[j] * b.array[i] + a.array[j + 1] * b.array[i + 5] + a.array[j + 2] * b.array[i + 10] + a.array[j + 3] * b.array[i + 15];
        for (int j = 0; j < 20; j += 5)
            t[j + 4] = a.array[j] * b.array[4] + a.array[j + 1] * b.array[9] + a.array[j + 2] * b.array[14] + a.array[j + 3] * b.array[19] + a.array[j + 4];
        set(t);
    }
    public void preConcat(ColorMatrix pre) { setConcat(this, pre); }
    public void postConcat(ColorMatrix post) { setConcat(post, this); }
}
