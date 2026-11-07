// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A bitmap as a fill. */
public class BitmapShader extends Shader {
    public BitmapShader(Bitmap bitmap, TileMode tileX, TileMode tileY) {
        kind = 4;
        this.bitmap = bitmap;
        tile = tileX;
        this.tileY = tileY;
    }
}
