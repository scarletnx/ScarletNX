// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.hwui;

/**
 * Fonts and text shaping, the HLE of minikin over HarfBuzz (src/android/hwui/text.c). A font is a handle of the
 * runtime; sizes are in pixels.
 */
public final class Text {
    private Text() {}

    /** A font file's face (index for a collection's): its handle, 0 when it can't be read. */
    public static synchronized native int loadFont(String path, int index);

    /** The metrics of a font at a size: ascent (negative), descent, leading, top, bottom, underline position and
     *  thickness, x-height, cap height. */
    public static native void metrics(int font, float size, float[] out);

    /**
     * The glyphs of text[start, start + count) in its context, shaped: their IDs into glyphs, their positions (x, y
     * pairs, from the run's start) into positions, each char's advance into advances (a cluster's on its first char,
     * 0 on the others; may be null). Its count of glyphs, and the run's advance in total[0]. Arrays too small get
     * nothing and the count still comes back.
     */
    public static native int shape(int font, float size, float scaleX, float letterSpacing, char[] text, int start,
                                   int count, int contextStart, int contextEnd, boolean rtl, int[] glyphs,
                                   float[] positions, float[] advances, float[] total);

    /** Whether the font has a glyph for the code point. */
    public static native boolean hasGlyph(int font, int codePoint);

    /** The bounds (left, top, right, bottom) of glyphs at positions, at a size. */
    public static native void bounds(int font, float size, int[] glyphs, float[] positions, int count, float[] out);

    /** The default font: the family the runtime ships, in snx.android.fonts. */
    private static int defaultFont;

    public static synchronized int defaultFont() {
        if (defaultFont == 0) {
            String dir = System.getProperty("snx.android.fonts", "android-fonts");
            defaultFont = loadFont(new java.io.File(dir, "NotoSansJP-Subset.ttf").getPath(), 0);
            if (defaultFont == 0) throw new IllegalStateException("snx: no font in " + dir);
        }
        return defaultFont;
    }
}
