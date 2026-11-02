// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** A width and a height in floats. */
public final class SizeF {
    private final float width, height;

    public SizeF(float width, float height) { this.width = width; this.height = height; }
    public float getWidth() { return width; }
    public float getHeight() { return height; }
    @Override public boolean equals(Object o) { return o instanceof SizeF s && s.width == width && s.height == height; }
    @Override public int hashCode() { return Float.floatToIntBits(width) ^ Float.floatToIntBits(height); }
    @Override public String toString() { return width + "x" + height; }
}
