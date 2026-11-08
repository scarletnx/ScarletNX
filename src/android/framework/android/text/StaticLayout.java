// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text laid out once, broken into lines for a width. */
public class StaticLayout extends Layout {
    public StaticLayout(CharSequence source, TextPaint paint, int width, Alignment align, float spacingmult, float spacingadd, boolean includepad) {
        this(source, 0, source.length(), paint, width, align, spacingmult, spacingadd, includepad);
    }

    public StaticLayout(CharSequence source, int bufstart, int bufend, TextPaint paint, int outerwidth, Alignment align,
                        float spacingmult, float spacingadd, boolean includepad) {
        this(source, bufstart, bufend, paint, outerwidth, align, spacingmult, spacingadd, includepad, null, 0);
    }

    public StaticLayout(CharSequence source, int bufstart, int bufend, TextPaint paint, int outerwidth, Alignment align,
                        float spacingmult, float spacingadd, boolean includepad, TextUtils.TruncateAt ellipsize, int ellipsizedWidth) {
        super(source, paint, outerwidth, align, spacingmult, spacingadd);
        generate(bufstart, bufend, ellipsize != null ? 1 : 0, ellipsize, ellipsizedWidth, includepad, null, null);
    }

    private StaticLayout(Builder b) {
        super(b.text, b.paint, b.width, b.alignment, b.spacingMult, b.spacingAdd);
        generate(b.start, b.end, b.maxLines, b.ellipsize, b.ellipsizedWidth, b.includePad, b.leftIndents, b.rightIndents);
    }

    public static final class Builder {
        CharSequence text;
        int start, end, width, maxLines = Integer.MAX_VALUE, ellipsizedWidth;
        TextPaint paint;
        Alignment alignment = Alignment.ALIGN_NORMAL;
        float spacingMult = 1, spacingAdd;
        boolean includePad = true;
        TextUtils.TruncateAt ellipsize;
        int[] leftIndents, rightIndents;

        private Builder() {}

        public static Builder obtain(CharSequence source, int start, int end, TextPaint paint, int width) {
            Builder b = new Builder();
            b.text = source;
            b.start = start;
            b.end = end;
            b.paint = paint;
            b.width = width;
            b.ellipsizedWidth = width;
            return b;
        }

        public Builder setText(CharSequence source) { return setText(source, 0, source.length()); }
        public Builder setText(CharSequence source, int start, int end) { text = source; this.start = start; this.end = end; return this; }
        public Builder setPaint(TextPaint paint) { this.paint = paint; return this; }
        public Builder setWidth(int width) { this.width = width; return this; }
        public Builder setAlignment(Alignment alignment) { this.alignment = alignment; return this; }
        public Builder setTextDirection(TextDirectionHeuristic textDir) { return this; }
        public Builder setLineSpacing(float spacingAdd, float spacingMult) { this.spacingAdd = spacingAdd; this.spacingMult = spacingMult; return this; }
        public Builder setIncludePad(boolean includePad) { this.includePad = includePad; return this; }
        public Builder setUseLineSpacingFromFallbacks(boolean useLineSpacingFromFallbacks) { return this; }
        public Builder setEllipsizedWidth(int ellipsizedWidth) { this.ellipsizedWidth = ellipsizedWidth; return this; }
        public Builder setEllipsize(TextUtils.TruncateAt ellipsize) { this.ellipsize = ellipsize; return this; }
        public Builder setMaxLines(int maxLines) { this.maxLines = maxLines; return this; }
        public Builder setBreakStrategy(int breakStrategy) { return this; }
        public Builder setHyphenationFrequency(int hyphenationFrequency) { return this; }
        public Builder setIndents(int[] leftIndents, int[] rightIndents) { this.leftIndents = leftIndents; this.rightIndents = rightIndents; return this; }
        public Builder setJustificationMode(int justificationMode) { return this; }
        public Builder setLineBreakConfig(android.graphics.text.LineBreakConfig lineBreakConfig) { return this; }
        public Builder setUseBoundsForWidth(boolean useBoundsForWidth) { return this; }
        public Builder setShiftDrawingOffsetForStartOverhang(boolean shift) { return this; }
        public Builder setMinimumFontMetrics(android.graphics.Paint.FontMetrics minimumFontMetrics) { return this; }
        public StaticLayout build() { return new StaticLayout(this); }
    }
}
