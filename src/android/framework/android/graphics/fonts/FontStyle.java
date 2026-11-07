// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.fonts;

/** A font's weight and slant. */
public final class FontStyle {
    public static final int FONT_WEIGHT_MIN = 1, FONT_WEIGHT_THIN = 100, FONT_WEIGHT_EXTRA_LIGHT = 200,
        FONT_WEIGHT_LIGHT = 300, FONT_WEIGHT_NORMAL = 400, FONT_WEIGHT_MEDIUM = 500, FONT_WEIGHT_SEMI_BOLD = 600,
        FONT_WEIGHT_BOLD = 700, FONT_WEIGHT_EXTRA_BOLD = 800, FONT_WEIGHT_BLACK = 900, FONT_WEIGHT_MAX = 1000;
    public static final int FONT_SLANT_UPRIGHT = 0, FONT_SLANT_ITALIC = 1;
    private final int weight, slant;

    public FontStyle() { this(FONT_WEIGHT_NORMAL, FONT_SLANT_UPRIGHT); }
    public FontStyle(int weight, int slant) { this.weight = weight; this.slant = slant; }
    public int getWeight() { return weight; }
    public int getSlant() { return slant; }
    public int getMatchScore(FontStyle o) { return Math.abs(weight - o.weight) / 100 + (slant == o.slant ? 0 : 2); }
    @Override public boolean equals(Object o) { return o instanceof FontStyle s && s.weight == weight && s.slant == slant; }
    @Override public int hashCode() { return weight * 2 + slant; }
}
