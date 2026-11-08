// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import android.content.res.AssetManager;
import android.graphics.fonts.FontFamily;
import java.io.File;

/**
 * A font family at a weight and slant. Every family is the face the runtime ships (snx.android.hwui.Text): a weight
 * of 600 or more without a face of its own is drawn bold by the renderer, and italic slanted, as Skia does.
 */
public class Typeface {
    public static final int NORMAL = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3;
    public static final Typeface DEFAULT = new Typeface(0, 400, false, "sans-serif");
    public static final Typeface DEFAULT_BOLD = new Typeface(0, 700, false, "sans-serif");
    public static final Typeface SANS_SERIF = DEFAULT;
    public static final Typeface SERIF = new Typeface(0, 400, false, "serif");
    public static final Typeface MONOSPACE = new Typeface(0, 400, false, "monospace");

    private int font; // the face, 0 for the default one (loaded when first drawn)
    private final int weight;
    private final boolean italic;
    private final String family;

    Typeface(int font, int weight, boolean italic, String family) {
        this.font = font;
        this.weight = weight;
        this.italic = italic;
        this.family = family;
    }

    /** The face's handle for the runtime. */
    public int font() {
        if (font == 0) font = snx.android.hwui.Text.defaultFont();
        return font;
    }

    public int getStyle() { return (weight >= 600 ? BOLD : 0) | (italic ? ITALIC : 0); }
    public final boolean isBold() { return weight >= 600; }
    public final boolean isItalic() { return italic; }
    public int getWeight() { return weight; }
    public String getSystemFontFamilyName() { return family; }

    public static Typeface create(String familyName, int style) {
        return new Typeface(0, (style & BOLD) != 0 ? 700 : 400, (style & ITALIC) != 0, familyName != null ? familyName : "sans-serif");
    }
    public static Typeface create(Typeface family, int style) {
        Typeface base = family != null ? family : DEFAULT;
        return new Typeface(base.font, (style & BOLD) != 0 ? 700 : 400, (style & ITALIC) != 0, base.family);
    }
    public static Typeface create(Typeface family, int weight, boolean italic) {
        Typeface base = family != null ? family : DEFAULT;
        return new Typeface(base.font, weight, italic, base.family);
    }
    public static Typeface defaultFromStyle(int style) { return create(DEFAULT, style); }
    public static Typeface createFromAsset(AssetManager mgr, String path) { return new Builder(mgr, path).build(); }
    public static Typeface createFromFile(File file) { return new Builder(file).build(); }
    public static Typeface createFromFile(String path) { return new Builder(new File(path)).build(); }
    @Override public boolean equals(Object o) { return o instanceof Typeface t && t.font == font && t.weight == weight && t.italic == italic && t.family.equals(family); }
    @Override public int hashCode() { return (font * 31 + weight) * 2 + (italic ? 1 : 0); }

    /** A typeface of a font file. */
    public static final class Builder {
        private String path;
        private int index, weight = -1, italic = -1;
        private byte[] data;
        private String fallback;

        public Builder(File path) { this.path = path.getPath(); }
        public Builder(java.io.FileDescriptor fd) { throw new UnsupportedOperationException("snx: Typeface.Builder of a descriptor"); }
        public Builder(String path) { this.path = path; }
        public Builder(AssetManager assets, String path) {
            data = snx.android.Apk.read("assets/" + path);
        }
        public Builder(AssetManager assets, String path, boolean isAsset, int cookie) {
            data = snx.android.Apk.read(isAsset ? "assets/" + path : path);
        }
        public Builder setWeight(int weight) { this.weight = weight; return this; }
        public Builder setItalic(boolean italic) { this.italic = italic ? 1 : 0; return this; }
        public Builder setTtcIndex(int ttcIndex) { index = ttcIndex; return this; }
        public Builder setFontVariationSettings(String settings) { return this; }
        public Builder setFontVariationSettings(android.graphics.fonts.FontVariationAxis[] axes) { return this; }
        public Builder setFallback(String familyName) { fallback = familyName; return this; }

        public Typeface build() {
            String file = path;
            if (data != null) file = android.graphics.fonts.Font.spill(data);
            int handle = file != null ? snx.android.hwui.Text.loadFont(file, index) : 0;
            if (handle == 0) return fallback != null ? create(fallback, NORMAL) : null;
            return new Typeface(handle, weight >= 0 ? weight : 400, italic == 1, "custom");
        }
    }

    /** A typeface of font families, the first one first. */
    public static final class CustomFallbackBuilder {
        private final FontFamily first;
        private android.graphics.fonts.FontStyle style = new android.graphics.fonts.FontStyle();
        private String systemFallback;

        public CustomFallbackBuilder(FontFamily family) { first = family; }
        public CustomFallbackBuilder setSystemFallback(String familyName) { systemFallback = familyName; return this; }
        public CustomFallbackBuilder setStyle(android.graphics.fonts.FontStyle style) { this.style = style; return this; }
        public CustomFallbackBuilder addCustomFallback(FontFamily family) { return this; }

        public Typeface build() {
            android.graphics.fonts.Font f = first.getFont(0);
            int handle = f != null ? f.handle() : 0;
            return new Typeface(handle, style.getWeight(), style.getSlant() == android.graphics.fonts.FontStyle.FONT_SLANT_ITALIC,
                systemFallback != null ? systemFallback : "custom");
        }
    }
}
