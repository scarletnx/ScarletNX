// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A filter of the colors a draw makes. Kinds the display list knows: a color blended in, a color matrix. */
public class ColorFilter {
    /** 0 none, 1 a color blended with mode, 2 a color matrix (20 floats). */
    int kind;
    int color;
    BlendMode mode = BlendMode.SRC_IN;
    float[] matrix;
}
