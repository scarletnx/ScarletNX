// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

import java.util.Locale;
import snx.android.hwui.DisplayList;
import snx.android.hwui.Text;

/**
 * How a draw looks: color, style and stroke, blending, filters and shaders, and text. Plain Java: a draw copies the
 * paint into the display list (write). Text is measured by the runtime's shaping (snx.android.hwui.Text).
 */
public class Paint {
    public static final int ANTI_ALIAS_FLAG = 0x01, FILTER_BITMAP_FLAG = 0x02, DITHER_FLAG = 0x04,
        UNDERLINE_TEXT_FLAG = 0x08, STRIKE_THRU_TEXT_FLAG = 0x10, FAKE_BOLD_TEXT_FLAG = 0x20, LINEAR_TEXT_FLAG = 0x40,
        SUBPIXEL_TEXT_FLAG = 0x80, EMBEDDED_BITMAP_TEXT_FLAG = 0x400;
    public static final int HINTING_OFF = 0, HINTING_ON = 1;
    public static final int BIDI_LTR = 0, BIDI_RTL = 1, BIDI_DEFAULT_LTR = 2, BIDI_DEFAULT_RTL = 3, BIDI_FORCE_LTR = 4,
        BIDI_FORCE_RTL = 5;
    public static final int CURSOR_AFTER = 0, CURSOR_AT_OR_AFTER = 1, CURSOR_BEFORE = 2, CURSOR_AT_OR_BEFORE = 3,
        CURSOR_AT = 4;
    public static final int START_HYPHEN_EDIT_NO_EDIT = 0, END_HYPHEN_EDIT_NO_EDIT = 0;
    public static final int TEXT_RUN_FLAG_LEFT_EDGE = 1, TEXT_RUN_FLAG_RIGHT_EDGE = 2;

    public enum Style { FILL, STROKE, FILL_AND_STROKE }
    public enum Cap { BUTT, ROUND, SQUARE }
    public enum Join { MITER, ROUND, BEVEL }
    public enum Align { LEFT, CENTER, RIGHT }

    public static class FontMetrics {
        public float top, ascent, descent, bottom, leading;
    }

    public static class FontMetricsInt {
        public int top, ascent, descent, bottom, leading;

        public void set(FontMetricsInt o) { top = o.top; ascent = o.ascent; descent = o.descent; bottom = o.bottom; leading = o.leading; }
        @Override public String toString() {
            return "FontMetricsInt: top=" + top + " ascent=" + ascent + " descent=" + descent + " bottom=" + bottom + " leading=" + leading;
        }
    }

    private int flags;
    private int color = 0xff000000;
    private Style style = Style.FILL;
    private Cap cap = Cap.BUTT;
    private Join join = Join.MITER;
    private float strokeWidth, miter = 4;
    private float textSize = 12, textScaleX = 1, textSkewX, letterSpacing, wordSpacing;
    private Typeface typeface;
    private Align align = Align.LEFT;
    private Shader shader;
    private ColorFilter colorFilter;
    private BlendMode blend;
    private Xfermode xfermode;
    private PathEffect pathEffect;
    private MaskFilter maskFilter;
    private float shadowRadius, shadowDx, shadowDy;
    private int shadowColor;
    private Locale locale = Locale.getDefault();
    private String fontFeatures, fontVariations;
    private int hinting = HINTING_ON;
    private int startHyphen, endHyphen;
    private boolean elegantTextHeight;
    private float underlinePosition, underlineThickness;

    public Paint() { this(ANTI_ALIAS_FLAG); }
    public Paint(int flags) { this.flags = flags | DITHER_FLAG; }
    public Paint(Paint paint) { set(paint); }

    public void reset() { set(new Paint()); }

    public void set(Paint src) {
        if (src == this) return;
        flags = src.flags; color = src.color; style = src.style; cap = src.cap; join = src.join;
        strokeWidth = src.strokeWidth; miter = src.miter; textSize = src.textSize; textScaleX = src.textScaleX;
        textSkewX = src.textSkewX; letterSpacing = src.letterSpacing; wordSpacing = src.wordSpacing;
        typeface = src.typeface; align = src.align; shader = src.shader; colorFilter = src.colorFilter;
        blend = src.blend; xfermode = src.xfermode; pathEffect = src.pathEffect; maskFilter = src.maskFilter;
        shadowRadius = src.shadowRadius; shadowDx = src.shadowDx; shadowDy = src.shadowDy;
        shadowColor = src.shadowColor; locale = src.locale; fontFeatures = src.fontFeatures;
        fontVariations = src.fontVariations; hinting = src.hinting; startHyphen = src.startHyphen;
        endHyphen = src.endHyphen; elegantTextHeight = src.elegantTextHeight;
    }

    public int getFlags() { return flags; }
    public void setFlags(int flags) { this.flags = flags; }
    public int getHinting() { return hinting; }
    public void setHinting(int mode) { hinting = mode; }
    public final boolean isAntiAlias() { return (flags & ANTI_ALIAS_FLAG) != 0; }
    public void setAntiAlias(boolean aa) { flag(ANTI_ALIAS_FLAG, aa); }
    public final boolean isDither() { return (flags & DITHER_FLAG) != 0; }
    public void setDither(boolean dither) { flag(DITHER_FLAG, dither); }
    public final boolean isLinearText() { return (flags & LINEAR_TEXT_FLAG) != 0; }
    public void setLinearText(boolean linearText) { flag(LINEAR_TEXT_FLAG, linearText); }
    public final boolean isSubpixelText() { return (flags & SUBPIXEL_TEXT_FLAG) != 0; }
    public void setSubpixelText(boolean subpixelText) { flag(SUBPIXEL_TEXT_FLAG, subpixelText); }
    public final boolean isUnderlineText() { return (flags & UNDERLINE_TEXT_FLAG) != 0; }
    public void setUnderlineText(boolean underlineText) { flag(UNDERLINE_TEXT_FLAG, underlineText); }
    public float getUnderlinePosition() { return metric(5); }
    public float getUnderlineThickness() { return metric(6); }
    public final boolean isStrikeThruText() { return (flags & STRIKE_THRU_TEXT_FLAG) != 0; }
    public void setStrikeThruText(boolean strikeThruText) { flag(STRIKE_THRU_TEXT_FLAG, strikeThruText); }
    public float getStrikeThruPosition() { return -metric(7) / 2; }
    public float getStrikeThruThickness() { return metric(6); }
    public final boolean isFakeBoldText() { return (flags & FAKE_BOLD_TEXT_FLAG) != 0; }
    public void setFakeBoldText(boolean fakeBoldText) { flag(FAKE_BOLD_TEXT_FLAG, fakeBoldText); }
    public final boolean isFilterBitmap() { return (flags & FILTER_BITMAP_FLAG) != 0; }
    public void setFilterBitmap(boolean filter) { flag(FILTER_BITMAP_FLAG, filter); }
    private void flag(int f, boolean on) { flags = on ? flags | f : flags & ~f; }

    public Style getStyle() { return style; }
    public void setStyle(Style style) { this.style = style; }
    public int getColor() { return color; }
    public long getColorLong() { return Color.pack(color); }
    public void setColor(int color) { this.color = color; }
    public void setColor(long color) { this.color = Color.toArgb(color); }
    public int getAlpha() { return color >>> 24; }
    public void setAlpha(int a) { color = (color & 0xffffff) | ((a & 0xff) << 24); }
    public void setARGB(int a, int r, int g, int b) { setColor((a << 24) | (r << 16) | (g << 8) | b); }
    public float getStrokeWidth() { return strokeWidth; }
    public void setStrokeWidth(float width) { if (width >= 0) strokeWidth = width; }
    public float getStrokeMiter() { return miter; }
    public void setStrokeMiter(float miter) { if (miter >= 0) this.miter = miter; }
    public Cap getStrokeCap() { return cap; }
    public void setStrokeCap(Cap cap) { this.cap = cap; }
    public Join getStrokeJoin() { return join; }
    public void setStrokeJoin(Join join) { this.join = join; }
    public boolean getFillPath(Path src, Path dst) { dst.set(src); return true; }
    public Shader getShader() { return shader; }
    public Shader setShader(Shader shader) { this.shader = shader; return shader; }
    public ColorFilter getColorFilter() { return colorFilter; }
    public ColorFilter setColorFilter(ColorFilter filter) { colorFilter = filter; return filter; }
    public Xfermode getXfermode() { return xfermode; }
    public Xfermode setXfermode(Xfermode xfermode) { this.xfermode = xfermode; blend = xfermode != null ? xfermode.mode : null; return xfermode; }
    public BlendMode getBlendMode() { return blend; }
    public void setBlendMode(BlendMode blendMode) { blend = blendMode; xfermode = null; }
    public PathEffect getPathEffect() { return pathEffect; }
    public PathEffect setPathEffect(PathEffect effect) { pathEffect = effect; return effect; }
    public MaskFilter getMaskFilter() { return maskFilter; }
    public MaskFilter setMaskFilter(MaskFilter maskfilter) { maskFilter = maskfilter; return maskfilter; }
    public Typeface getTypeface() { return typeface; }
    public Typeface setTypeface(Typeface typeface) { this.typeface = typeface; return typeface; }
    public void setShadowLayer(float radius, float dx, float dy, int shadowColor) {
        shadowRadius = radius; shadowDx = dx; shadowDy = dy; this.shadowColor = shadowColor;
    }
    public void setShadowLayer(float radius, float dx, float dy, long shadowColor) { setShadowLayer(radius, dx, dy, Color.toArgb(shadowColor)); }
    public void clearShadowLayer() { setShadowLayer(0, 0, 0, 0); }
    public boolean hasShadowLayer() { return shadowRadius > 0; }
    public float getShadowLayerRadius() { return shadowRadius; }
    public float getShadowLayerDx() { return shadowDx; }
    public float getShadowLayerDy() { return shadowDy; }
    public int getShadowLayerColor() { return shadowColor; }
    public long getShadowLayerColorLong() { return Color.pack(shadowColor); }
    public Align getTextAlign() { return align; }
    public void setTextAlign(Align align) { this.align = align; }
    public Locale getTextLocale() { return locale; }
    public android.os.LocaleList getTextLocales() { return new android.os.LocaleList(locale); }
    public void setTextLocale(Locale locale) { if (locale != null) this.locale = locale; }
    public void setTextLocales(android.os.LocaleList locales) { if (locales != null && locales.size() > 0) locale = locales.get(0); }
    public boolean isElegantTextHeight() { return elegantTextHeight; }
    public void setElegantTextHeight(boolean elegant) { elegantTextHeight = elegant; }
    public float getTextSize() { return textSize; }
    public void setTextSize(float textSize) { if (textSize > 0) this.textSize = textSize; }
    public float getTextScaleX() { return textScaleX; }
    public void setTextScaleX(float scaleX) { textScaleX = scaleX; }
    public float getTextSkewX() { return textSkewX; }
    public void setTextSkewX(float skewX) { textSkewX = skewX; }
    public float getLetterSpacing() { return letterSpacing; }
    public void setLetterSpacing(float letterSpacing) { this.letterSpacing = letterSpacing; }
    public float getWordSpacing() { return wordSpacing; }
    public void setWordSpacing(float wordSpacing) { this.wordSpacing = wordSpacing; }
    public String getFontFeatureSettings() { return fontFeatures; }
    public void setFontFeatureSettings(String settings) { fontFeatures = settings == null || settings.isEmpty() ? null : settings; }
    public String getFontVariationSettings() { return fontVariations; }
    public boolean setFontVariationSettings(String settings) { fontVariations = settings; return true; }
    public int getStartHyphenEdit() { return startHyphen; }
    public int getEndHyphenEdit() { return endHyphen; }
    public void setStartHyphenEdit(int edit) { startHyphen = edit; }
    public void setEndHyphenEdit(int edit) { endHyphen = edit; }

    // ---- text ----

    /** The face drawn and measured. */
    public int font() { return (typeface != null ? typeface : Typeface.DEFAULT).font(); }
    /** Whether the renderer draws the text bold: a fake bold flag, or a weight the face does not have. */
    public boolean fakeBold() { return isFakeBoldText() || (typeface != null && typeface.getWeight() >= 600); }
    /** The slant the renderer adds: the paint's, and an italic face's. */
    public float skew() { return textSkewX + (typeface != null && typeface.isItalic() ? -0.25f : 0); }

    private float metric(int index) {
        float[] m = new float[9];
        Text.metrics(font(), textSize, m);
        return m[index];
    }

    public float ascent() { return metric(0); }
    public float descent() { return metric(1); }
    public float getFontSpacing() { return getFontMetrics(null); }

    public float getFontMetrics(FontMetrics metrics) {
        float[] m = new float[9];
        Text.metrics(font(), textSize, m);
        if (metrics != null) { metrics.ascent = m[0]; metrics.descent = m[1]; metrics.leading = m[2]; metrics.top = m[3]; metrics.bottom = m[4]; }
        return m[1] - m[0] + m[2];
    }
    public FontMetrics getFontMetrics() { FontMetrics fm = new FontMetrics(); getFontMetrics(fm); return fm; }
    public void getFontMetricsForLocale(FontMetrics metrics) { getFontMetrics(metrics); }

    public int getFontMetricsInt(FontMetricsInt fmi) {
        float[] m = new float[9];
        Text.metrics(font(), textSize, m);
        if (fmi != null) {
            fmi.ascent = (int) Math.floor(m[0]); fmi.descent = (int) Math.ceil(m[1]); fmi.leading = (int) Math.ceil(m[2]);
            fmi.top = (int) Math.floor(m[3]); fmi.bottom = (int) Math.ceil(m[4]);
        }
        return (int) Math.ceil(m[1]) - (int) Math.floor(m[0]) + (int) Math.ceil(m[2]);
    }
    public FontMetricsInt getFontMetricsInt() { FontMetricsInt fm = new FontMetricsInt(); getFontMetricsInt(fm); return fm; }
    public void getFontMetricsInt(CharSequence text, int start, int count, int contextStart, int contextCount,
                                  boolean isRtl, FontMetricsInt outMetrics) { getFontMetricsInt(outMetrics); }
    public void getFontMetricsInt(char[] text, int start, int count, int contextStart, int contextCount,
                                  boolean isRtl, FontMetricsInt outMetrics) { getFontMetricsInt(outMetrics); }

    /** The run text[start, start + count) shaped in its context: its advances per char into advances. */
    public float run(char[] text, int start, int count, int ctxStart, int ctxEnd, boolean rtl, float[] advances, int advIndex) {
        if (count <= 0) return 0;
        float[] total = new float[1];
        float[] adv = advances != null ? new float[count] : null;
        Text.shape(font(), textSize, textScaleX, letterSpacing, text, start, count, ctxStart, ctxEnd, rtl, null, null, adv, total);
        if (advances != null) System.arraycopy(adv, 0, advances, advIndex, count);
        return total[0];
    }

    private static char[] chars(CharSequence text, int start, int end) {
        char[] c = new char[end - start];
        android.text.TextUtils.getChars(text, start, end, c, 0);
        return c;
    }

    public float measureText(char[] text, int index, int count) { return run(text, index, count, index, index + count, false, null, 0); }
    public float measureText(String text, int start, int end) { return measureText(chars(text, start, end), 0, end - start); }
    public float measureText(String text) { return measureText(text, 0, text.length()); }
    public float measureText(CharSequence text, int start, int end) { return measureText(chars(text, start, end), 0, end - start); }

    public int getTextWidths(char[] text, int index, int count, float[] widths) {
        run(text, index, count, index, index + count, false, widths, 0);
        return count;
    }
    public int getTextWidths(CharSequence text, int start, int end, float[] widths) { return getTextWidths(chars(text, start, end), 0, end - start, widths); }
    public int getTextWidths(String text, int start, int end, float[] widths) { return getTextWidths(chars(text, start, end), 0, end - start, widths); }
    public int getTextWidths(String text, float[] widths) { return getTextWidths(text, 0, text.length(), widths); }

    public float getTextRunAdvances(char[] chars, int index, int count, int contextIndex, int contextCount,
                                    boolean isRtl, float[] advances, int advancesIndex) {
        return run(chars, index, count, contextIndex, contextIndex + contextCount, isRtl, advances, advancesIndex);
    }

    public float getTextRunAdvances(CharSequence text, int start, int end, int contextStart, int contextEnd,
                                    boolean isRtl, float[] advances, int advancesIndex) {
        char[] c = chars(text, contextStart, contextEnd);
        return run(c, start - contextStart, end - start, 0, c.length, isRtl, advances, advancesIndex);
    }

    public float getRunAdvance(char[] text, int start, int end, int contextStart, int contextEnd, boolean isRtl, int offset) {
        if (offset <= start) return 0;
        float[] adv = new float[end - start];
        run(text, start, end - start, contextStart, contextEnd, isRtl, adv, 0);
        float sum = 0;
        for (int i = 0; i < offset - start && i < adv.length; ++i) sum += adv[i];
        return sum;
    }

    public float getRunAdvance(CharSequence text, int start, int end, int contextStart, int contextEnd, boolean isRtl, int offset) {
        char[] c = chars(text, contextStart, contextEnd);
        return getRunAdvance(c, start - contextStart, end - contextStart, 0, c.length, isRtl, offset - contextStart);
    }

    public float getRunCharacterAdvance(char[] text, int start, int end, int contextStart, int contextEnd,
                                        boolean isRtl, int offset, float[] advances, int advancesIndex) {
        if (advances != null) run(text, start, end - start, contextStart, contextEnd, isRtl, advances, advancesIndex);
        return getRunAdvance(text, start, end, contextStart, contextEnd, isRtl, offset);
    }

    public int getOffsetForAdvance(char[] text, int start, int end, int contextStart, int contextEnd, boolean isRtl, float advance) {
        float[] adv = new float[end - start];
        run(text, start, end - start, contextStart, contextEnd, isRtl, adv, 0);
        float sum = 0;
        for (int i = 0; i < adv.length; ++i) {
            if (sum + adv[i] / 2 > advance) return start + i;
            sum += adv[i];
        }
        return end;
    }

    public int getOffsetForAdvance(CharSequence text, int start, int end, int contextStart, int contextEnd, boolean isRtl, float advance) {
        char[] c = chars(text, contextStart, contextEnd);
        return contextStart + getOffsetForAdvance(c, start - contextStart, end - contextStart, 0, c.length, isRtl, advance);
    }

    public int getTextRunCursor(char[] text, int contextStart, int contextLength, boolean isRtl, int offset, int cursorOpt) {
        int end = contextStart + contextLength;
        switch (cursorOpt) {
            case CURSOR_AFTER: return Math.min(offset + 1, end);
            case CURSOR_BEFORE: return Math.max(offset - 1, contextStart);
            default: return offset;
        }
    }

    public int getTextRunCursor(CharSequence text, int contextStart, int contextEnd, boolean isRtl, int offset, int cursorOpt) {
        return getTextRunCursor(chars(text, 0, text.length()), contextStart, contextEnd - contextStart, isRtl, offset, cursorOpt);
    }

    public int breakText(char[] text, int index, int count, float maxWidth, float[] measuredWidth) {
        boolean back = count < 0;
        int n = Math.abs(count);
        float[] adv = new float[n];
        run(text, back ? index - n + 1 : index, n, 0, text.length, false, adv, 0);
        float sum = 0;
        int i = 0;
        for (; i < n; ++i) {
            float a = adv[back ? n - 1 - i : i];
            if (sum + a > maxWidth) break;
            sum += a;
        }
        if (measuredWidth != null) measuredWidth[0] = sum;
        return i;
    }

    public int breakText(CharSequence text, int start, int end, boolean measureForwards, float maxWidth, float[] measuredWidth) {
        char[] c = chars(text, start, end);
        return breakText(c, measureForwards ? 0 : c.length - 1, measureForwards ? c.length : -c.length, maxWidth, measuredWidth);
    }

    public int breakText(String text, boolean measureForwards, float maxWidth, float[] measuredWidth) {
        return breakText(text, 0, text.length(), measureForwards, maxWidth, measuredWidth);
    }

    public void getTextBounds(char[] text, int index, int count, Rect bounds) {
        if (count <= 0) { bounds.setEmpty(); return; }
        int[] glyphs = new int[count * 2];
        float[] pos = new float[count * 4];
        float[] total = new float[1];
        int n = Text.shape(font(), textSize, textScaleX, letterSpacing, text, index, count, index, index + count, false, glyphs, pos, null, total);
        if (n > glyphs.length) {
            glyphs = new int[n];
            pos = new float[2 * n];
            Text.shape(font(), textSize, textScaleX, letterSpacing, text, index, count, index, index + count, false, glyphs, pos, null, total);
        }
        float[] b = new float[4];
        Text.bounds(font(), textSize, glyphs, pos, n, b);
        bounds.set((int) Math.floor(b[0]), (int) Math.floor(b[1]), (int) Math.ceil(b[2]), (int) Math.ceil(b[3]));
    }

    public void getTextBounds(String text, int start, int end, Rect bounds) { getTextBounds(chars(text, start, end), 0, end - start, bounds); }
    public void getTextBounds(CharSequence text, int start, int end, Rect bounds) { getTextBounds(chars(text, start, end), 0, end - start, bounds); }

    public void getTextPath(char[] text, int index, int count, float x, float y, Path path) {
        Rect r = new Rect();
        getTextBounds(text, index, count, r);
        r.offset((int) x, (int) y);
        path.addRect(new RectF(r), Path.Direction.CW);
    }
    public void getTextPath(String text, int start, int end, float x, float y, Path path) { getTextPath(chars(text, start, end), 0, end - start, x, y, path); }

    public boolean hasGlyph(String string) {
        for (int i = 0; i < string.length(); ) {
            int cp = string.codePointAt(i);
            if (!Text.hasGlyph(font(), cp)) return false;
            i += Character.charCount(cp);
        }
        return true;
    }

    public float getUnderlinePositionInternal() { return metric(5); }

    // ---- the display list ----

    /** The paint into a display list: the fields the renderer reads (docs/android/HWUI.md). */
    public void write(DisplayList.Writer w) {
        w.i32(color);
        w.u8(style.ordinal());
        w.u8(cap.ordinal());
        w.u8(join.ordinal());
        w.u8(flags & 0xff);
        w.f32(strokeWidth);
        w.f32(miter);
        w.u8(blend != null ? blend.skia() : BlendMode.SRC_OVER.skia());
        Shader s = shader != null && shader.kind != 0 ? shader : null;
        ColorFilter f = colorFilter != null && colorFilter.kind != 0 ? colorFilter : null;
        float[] dash = pathEffect != null ? pathEffect.intervals : null;
        w.u8(s != null ? s.kind : 0);
        w.u8(f != null ? f.kind : 0);
        w.u8(dash != null ? dash.length : 0);
        if (s != null) {
            w.u8(s.tile.ordinal());
            w.u8(s.tileY.ordinal());
            w.u16(s.colors.length);
            for (int i = 0; i < 6; ++i) w.f32(i < s.points.length ? s.points[i] : 0);
            for (float v : s.localMatrix().values()) w.f32(v);
            for (int c : s.colors) w.i32(c);
            w.u8(s.positions != null ? 1 : 0);
            w.u8(0); w.u8(0); w.u8(0);
            if (s.positions != null) for (int i = 0; i < s.colors.length; ++i) w.f32(i < s.positions.length ? s.positions[i] : 1);
            w.i32(s.bitmap != null ? s.bitmap.id() : 0);
        }
        if (f != null) {
            if (f.kind == 1) {
                w.i32(f.color);
                w.u8(f.mode.skia());
                w.u8(0); w.u8(0); w.u8(0);
            } else {
                for (int i = 0; i < 20; ++i) w.f32(f.matrix[i]);
            }
        }
        if (dash != null) {
            w.f32(pathEffect.phase);
            for (float v : dash) w.f32(v);
        }
    }

    /** Whether a draw with the paint shows nothing (transparent, no shader or filter, source-over). */
    public boolean nothingToDraw() {
        return (color >>> 24) == 0 && shader == null && colorFilter == null
            && (blend == null || blend == BlendMode.SRC_OVER || blend == BlendMode.DST || blend == BlendMode.DST_OVER);
    }
}
