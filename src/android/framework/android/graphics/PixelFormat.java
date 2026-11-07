// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** Pixel format constants. */
public class PixelFormat {
    public static final int UNKNOWN = 0, TRANSLUCENT = -3, TRANSPARENT = -2, OPAQUE = -1, RGBA_8888 = 1, RGBX_8888 = 2,
        RGB_888 = 3, RGB_565 = 4, RGBA_F16 = 0x16, RGBA_1010102 = 0x2b;

    public int bytesPerPixel, bitsPerPixel;

    public static boolean formatHasAlpha(int format) {
        return format == TRANSLUCENT || format == TRANSPARENT || format == RGBA_8888 || format == RGBA_F16 || format == RGBA_1010102;
    }
}
