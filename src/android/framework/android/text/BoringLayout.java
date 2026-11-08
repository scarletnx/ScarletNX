// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

import android.graphics.Paint;

/** One line of plain text. */
public class BoringLayout extends Layout {
    public static class Metrics extends Paint.FontMetricsInt {
        public int width;

        @Override public String toString() { return super.toString() + " width=" + width; }
    }

    public BoringLayout(CharSequence source, TextPaint paint, int outerwidth, Alignment align, float spacingMult,
                        float spacingAdd, Metrics metrics, boolean includePad) {
        this(source, paint, outerwidth, align, spacingMult, spacingAdd, metrics, includePad, null, outerwidth);
    }

    public BoringLayout(CharSequence source, TextPaint paint, int outerwidth, Alignment align, float spacingMult,
                        float spacingAdd, Metrics metrics, boolean includePad, TextUtils.TruncateAt ellipsize, int ellipsizedWidth) {
        super(source, paint, outerwidth, align, spacingMult, spacingAdd);
        generate(0, source.length(), 1, ellipsize, ellipsizedWidth, includePad, null, null);
    }

    public BoringLayout(CharSequence source, TextPaint paint, int outerWidth, Alignment align, float spacingMult,
                        float spacingAdd, Metrics metrics, boolean includePad, TextUtils.TruncateAt ellipsize,
                        int ellipsizedWidth, boolean useFallbackLineSpacing) {
        this(source, paint, outerWidth, align, spacingMult, spacingAdd, metrics, includePad, ellipsize, ellipsizedWidth);
    }

    public static BoringLayout make(CharSequence source, TextPaint paint, int outerWidth, Alignment align, float spacingMult,
                                    float spacingAdd, Metrics metrics, boolean includePad) {
        return new BoringLayout(source, paint, outerWidth, align, spacingMult, spacingAdd, metrics, includePad);
    }

    public static Metrics isBoring(CharSequence text, TextPaint paint) { return isBoring(text, paint, null); }

    public static Metrics isBoring(CharSequence text, TextPaint paint, Metrics metrics) {
        return isBoring(text, paint, TextDirectionHeuristics.FIRSTSTRONG_LTR, false, metrics);
    }

    public static Metrics isBoring(CharSequence text, TextPaint paint, TextDirectionHeuristic textDir, Metrics metrics) {
        return isBoring(text, paint, textDir, false, metrics);
    }

    /** The metrics of one line when the text is plain: no newline or tab, no right to left, no paragraph spans. */
    public static Metrics isBoring(CharSequence text, TextPaint paint, TextDirectionHeuristic textDir,
                                   boolean useFallbackLineSpacing, Metrics metrics) {
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c == '\n' || c == '\t' || (c >= 0x0590 && c <= 0x08ff) || (c >= 0xd800 && c <= 0xdfff) || (c >= 0xfb1d && c <= 0xfefc))
                return null;
        }
        if (text instanceof Spanned s && s.getSpans(0, text.length(), android.text.style.ParagraphStyle.class).length > 0) return null;
        if (text instanceof Spanned s && s.getSpans(0, text.length(), android.text.style.MetricAffectingSpan.class).length > 0) return null;
        Metrics m = metrics != null ? metrics : new Metrics();
        paint.getFontMetricsInt(m);
        m.width = (int) Math.ceil(paint.measureText(text, 0, text.length()));
        return m;
    }

    public static Metrics isBoring(CharSequence text, TextPaint paint, TextDirectionHeuristic textDir,
                                   boolean useFallbackLineSpacing, Paint.FontMetrics minimumFontMetrics, Metrics metrics) {
        return isBoring(text, paint, textDir, useFallbackLineSpacing, metrics);
    }
}
