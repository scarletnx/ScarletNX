// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A color blended into each pixel with a Porter-Duff mode. */
public class PorterDuffColorFilter extends ColorFilter {
    private final PorterDuff.Mode pdMode;

    public PorterDuffColorFilter(int color, PorterDuff.Mode mode) {
        kind = 1;
        this.color = color;
        pdMode = mode;
        this.mode = BlendMode.fromPorterDuff(mode);
    }

    public int getColor() { return color; }
    public PorterDuff.Mode getMode() { return pdMode; }
    @Override public boolean equals(Object o) { return o instanceof PorterDuffColorFilter f && f.color == color && f.pdMode == pdMode; }
    @Override public int hashCode() { return 31 * pdMode.hashCode() + color; }
}
