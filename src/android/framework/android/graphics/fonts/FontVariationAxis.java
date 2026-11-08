// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.fonts;

/** A variable font's axis and its value. */
public final class FontVariationAxis {
    private final String tag;
    private final float value;

    public FontVariationAxis(String tagString, float styleValue) { tag = tagString; value = styleValue; }
    public String getTag() { return tag; }
    public float getStyleValue() { return value; }
    public static FontVariationAxis[] fromFontVariationSettings(String settings) { return new FontVariationAxis[0]; }
    public static String toFontVariationSettings(FontVariationAxis[] axes) { return ""; }
    @Override public String toString() { return "'" + tag + "' " + value; }
}
