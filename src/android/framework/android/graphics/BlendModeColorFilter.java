// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A color blended into each pixel with a blend mode. */
public final class BlendModeColorFilter extends ColorFilter {
    public BlendModeColorFilter(int color, BlendMode mode) {
        kind = 1;
        this.color = color;
        this.mode = mode;
    }

    public int getColor() { return color; }
    public BlendMode getMode() { return mode; }
    @Override public boolean equals(Object o) { return o instanceof BlendModeColorFilter f && f.color == color && f.mode == mode; }
    @Override public int hashCode() { return 31 * mode.hashCode() + color; }
}
