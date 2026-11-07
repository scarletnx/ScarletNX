// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** A width and a height in pixels. */
public final class Size {
    private final int width, height;

    public Size(int width, int height) { this.width = width; this.height = height; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    @Override public boolean equals(Object o) { return o instanceof Size s && s.width == width && s.height == height; }
    @Override public int hashCode() { return height ^ ((width << 16) | (width >>> 16)); }
    @Override public String toString() { return width + "x" + height; }
    public static Size parseSize(String s) {
        int x = s.indexOf('x') >= 0 ? s.indexOf('x') : s.indexOf('*');
        if (x < 0) throw new NumberFormatException("Invalid Size: \"" + s + "\"");
        return new Size(Integer.parseInt(s.substring(0, x)), Integer.parseInt(s.substring(x + 1)));
    }
}
