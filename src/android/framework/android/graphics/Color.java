// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** ARGB colors as ints. */
public class Color {
    public static final int BLACK = 0xff000000, DKGRAY = 0xff444444, GRAY = 0xff888888, LTGRAY = 0xffcccccc,
        WHITE = 0xffffffff, RED = 0xffff0000, GREEN = 0xff00ff00, BLUE = 0xff0000ff, YELLOW = 0xffffff00,
        CYAN = 0xff00ffff, MAGENTA = 0xffff00ff, TRANSPARENT = 0;

    /** The components of the color space, then alpha */
    private final float[] c;
    private final ColorSpace cs;

    public Color() { this(0, 0, 0, 1); }
    private Color(float r, float g, float b, float a) { this(new float[] {r, g, b, a}, ColorSpace.get(ColorSpace.Named.SRGB)); }
    private Color(float[] c, ColorSpace cs) { this.c = c; this.cs = cs; }

    public static int alpha(int color) { return color >>> 24; }
    public static int red(int color) { return (color >> 16) & 0xff; }
    public static int green(int color) { return (color >> 8) & 0xff; }
    public static int blue(int color) { return color & 0xff; }
    public static int rgb(int red, int green, int blue) { return 0xff000000 | (red << 16) | (green << 8) | blue; }
    public static int rgb(float red, float green, float blue) { return argb(1f, red, green, blue); }
    public static int argb(int alpha, int red, int green, int blue) { return (alpha << 24) | (red << 16) | (green << 8) | blue; }
    public static int argb(float alpha, float red, float green, float blue) {
        return ((int) (alpha * 255 + 0.5f) << 24) | ((int) (red * 255 + 0.5f) << 16) | ((int) (green * 255 + 0.5f) << 8) | (int) (blue * 255 + 0.5f);
    }
    public static float luminance(int color) {
        double r = lin(red(color) / 255.0), g = lin(green(color) / 255.0), b = lin(blue(color) / 255.0);
        return (float) (0.2126 * r + 0.7152 * g + 0.0722 * b);
    }
    public static float luminance(long color) {
        if (colorSpace(color).getModel() != ColorSpace.Model.RGB) throw new IllegalArgumentException("The specified color must be encoded in an RGB color space");
        double l = 0.2126 * lin(red(color)) + 0.7152 * lin(green(color)) + 0.0722 * lin(blue(color));
        return (float) Math.max(0, Math.min(1, l));
    }
    public static boolean isWideGamut(long color) { return colorSpace(color).isWideGamut(); }
    private static double lin(double c) { return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }

    /** A color long in sRGB: the int's components as Android's half-float-free encoding of sRGB colors. */
    public static long pack(int color) { return (color & 0xffffffffL) << 32; }
    public static long pack(float red, float green, float blue) { return pack(red, green, blue, 1f); }
    public static long pack(float red, float green, float blue, float alpha) { return pack(argb(alpha, red, green, blue)); }
    public static long pack(float red, float green, float blue, float alpha, ColorSpace colorSpace) {
        return pack(red, green, blue, alpha);
    }
    public static int toArgb(long color) {
        if ((color & 0x3fL) == 0) return (int) (color >>> 32);
        return argb(alpha(color), red(color), green(color), blue(color));
    }
    public static long convert(long color, ColorSpace colorSpace) { return color; }
    public static long convert(int color, ColorSpace colorSpace) { return pack(color); }
    public static boolean isSrgb(long color) { return (color & 0x3fL) == 0; }
    public static boolean isInColorSpace(long color, ColorSpace cs) { return true; }
    public static ColorSpace colorSpace(long color) { return ColorSpace.get(ColorSpace.Named.SRGB); }

    /** A color long's components: sRGB ones as 8-bit, others (from the colorspace's half floats) as Android packs them. */
    public static float red(long color) { return isSrgb(color) ? ((color >> 48) & 0xff) / 255f : half((short) (color >> 48)); }
    public static float green(long color) { return isSrgb(color) ? ((color >> 40) & 0xff) / 255f : half((short) (color >> 32)); }
    public static float blue(long color) { return isSrgb(color) ? ((color >> 32) & 0xff) / 255f : half((short) (color >> 16)); }
    public static float alpha(long color) { return isSrgb(color) ? ((color >> 56) & 0xff) / 255f : ((color >> 6) & 0x3ff) / 1023f; }
    static float half(short h) { return Float.float16ToFloat(h); }

    public static Color valueOf(int color) { return new Color(red(color) / 255f, green(color) / 255f, blue(color) / 255f, alpha(color) / 255f); }
    public static Color valueOf(long color) { return new Color(red(color), green(color), blue(color), alpha(color)); }
    public static Color valueOf(float r, float g, float b) { return new Color(r, g, b, 1); }
    public static Color valueOf(float r, float g, float b, float a) { return new Color(r, g, b, a); }
    public static Color valueOf(float[] components, ColorSpace colorSpace) {
        int n = colorSpace.getComponentCount() + 1;
        if (components.length < n) throw new IllegalArgumentException("Received a component array of length " + components.length + " but the color model requires " + n + " (including alpha)");
        return new Color(java.util.Arrays.copyOf(components, n), colorSpace);
    }
    public int toArgb() { return argb(alpha(), red(), green(), blue()); }
    public long pack() { return pack(toArgb()); }
    public float red() { return c[0]; }
    public float green() { return c[1]; }
    public float blue() { return c[2]; }
    public float alpha() { return c[c.length - 1]; }
    public ColorSpace getColorSpace() { return cs; }
    public ColorSpace.Model getModel() { return cs.getModel(); }
    public int getComponentCount() { return c.length; }
    public float getComponent(int component) { return c[component]; }
    public float[] getComponents() { return c.clone(); }
    /** The color in another color space, through XYZ (the color spaces here keep sRGB's values). */
    public Color convert(ColorSpace colorSpace) {
        float[] v = colorSpace.fromXyz(cs.toXyz(java.util.Arrays.copyOf(c, c.length - 1)));
        float[] out = java.util.Arrays.copyOf(v, colorSpace.getComponentCount() + 1);
        out[out.length - 1] = alpha();
        return new Color(out, colorSpace);
    }

    public static void RGBToHSV(int red, int green, int blue, float[] hsv) {
        float r = red / 255f, g = green / 255f, b = blue / 255f, max = Math.max(r, Math.max(g, b)), min = Math.min(r, Math.min(g, b)), d = max - min;
        float h = d == 0 ? 0 : max == r ? ((g - b) / d) % 6 : max == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h *= 60;
        if (h < 0) h += 360;
        hsv[0] = h; hsv[1] = max == 0 ? 0 : d / max; hsv[2] = max;
    }
    public static void colorToHSV(int color, float[] hsv) { RGBToHSV(red(color), green(color), blue(color), hsv); }
    public static int HSVToColor(float[] hsv) { return HSVToColor(0xff, hsv); }
    public static int HSVToColor(int alpha, float[] hsv) {
        float h = ((hsv[0] % 360) + 360) % 360, s = hsv[1], v = hsv[2], c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c;
        float r, g, b;
        if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
        else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
        return argb(alpha, Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255));
    }

    public static int parseColor(String s) {
        if (s.charAt(0) == '#') {
            long c = Long.parseLong(s.substring(1), 16);
            if (s.length() == 7) c |= 0xff000000L;
            else if (s.length() != 9) throw new IllegalArgumentException("Unknown color");
            return (int) c;
        }
        switch (s.toLowerCase(java.util.Locale.ROOT)) {
            case "black": return BLACK;
            case "darkgray": return DKGRAY;
            case "gray": return GRAY;
            case "lightgray": return LTGRAY;
            case "white": return WHITE;
            case "red": return RED;
            case "green": return GREEN;
            case "blue": return BLUE;
            case "yellow": return YELLOW;
            case "cyan": return CYAN;
            case "magenta": return MAGENTA;
            default: throw new IllegalArgumentException("Unknown color");
        }
    }
}
