// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text measured ahead of its layout (here, measured with its layout). */
public class PrecomputedText implements Spannable {
    private final SpannableString text;
    private final Params params;
    /** The end of each paragraph, after its line feed */
    private final int[] paragraphEnds;

    private PrecomputedText(CharSequence text, Params params) {
        this.text = new SpannableString(text);
        this.params = params;
        int n = 0, len = text.length();
        int[] ends = new int[4];
        for (int start = 0; start < len; ) {
            int lf = TextUtils.indexOf(text, '\n', start);
            int end = lf < 0 ? len : lf + 1;
            if (n == ends.length) ends = java.util.Arrays.copyOf(ends, n * 2);
            ends[n++] = end;
            start = end;
        }
        paragraphEnds = java.util.Arrays.copyOf(ends, n);
    }
    public static PrecomputedText create(CharSequence text, Params params) { return new PrecomputedText(text, params); }
    public Params getParams() { return params; }
    public int getParagraphCount() { return paragraphEnds.length; }
    public int getParagraphStart(int paraIndex) {
        checkParagraph(paraIndex);
        return paraIndex == 0 ? 0 : paragraphEnds[paraIndex - 1];
    }
    public int getParagraphEnd(int paraIndex) {
        checkParagraph(paraIndex);
        return paragraphEnds[paraIndex];
    }
    private void checkParagraph(int i) {
        if (i < 0 || i >= paragraphEnds.length) throw new IllegalArgumentException("paraIndex must be in [0, " + paragraphEnds.length + ")");
    }
    @Override public int length() { return text.length(); }
    @Override public char charAt(int i) { return text.charAt(i); }
    @Override public CharSequence subSequence(int start, int end) { return text.subSequence(start, end); }
    @Override public String toString() { return text.toString(); }
    @Override public void setSpan(Object what, int start, int end, int flags) { text.setSpan(what, start, end, flags); }
    @Override public void removeSpan(Object what) { text.removeSpan(what); }
    @Override public <T> T[] getSpans(int start, int end, Class<T> type) { return text.getSpans(start, end, type); }
    @Override public int getSpanStart(Object tag) { return text.getSpanStart(tag); }
    @Override public int getSpanEnd(Object tag) { return text.getSpanEnd(tag); }
    @Override public int getSpanFlags(Object tag) { return text.getSpanFlags(tag); }
    @Override public int nextSpanTransition(int start, int limit, Class type) { return text.nextSpanTransition(start, limit, type); }

    public static final class Params {
        private final TextPaint paint;
        private final TextDirectionHeuristic textDir;
        private final int breakStrategy, hyphenationFrequency;

        Params(TextPaint paint, TextDirectionHeuristic textDir, int breakStrategy, int hyphenationFrequency) {
            this.paint = paint;
            this.textDir = textDir;
            this.breakStrategy = breakStrategy;
            this.hyphenationFrequency = hyphenationFrequency;
        }
        public TextPaint getTextPaint() { return paint; }
        public TextDirectionHeuristic getTextDirection() { return textDir; }
        public int getBreakStrategy() { return breakStrategy; }
        public int getHyphenationFrequency() { return hyphenationFrequency; }

        public static class Builder {
            private final TextPaint paint;
            private TextDirectionHeuristic textDir = TextDirectionHeuristics.FIRSTSTRONG_LTR;
            private int breakStrategy = Layout.BREAK_STRATEGY_HIGH_QUALITY, hyphenationFrequency = Layout.HYPHENATION_FREQUENCY_NORMAL;

            public Builder(TextPaint paint) { this.paint = paint; }
            public Builder(Params params) {
                paint = params.paint;
                textDir = params.textDir;
                breakStrategy = params.breakStrategy;
                hyphenationFrequency = params.hyphenationFrequency;
            }
            public Builder setBreakStrategy(int strategy) { breakStrategy = strategy; return this; }
            public Builder setHyphenationFrequency(int frequency) { hyphenationFrequency = frequency; return this; }
            public Builder setTextDirection(TextDirectionHeuristic textDir) { this.textDir = textDir; return this; }
            public Params build() { return new Params(paint, textDir, breakStrategy, hyphenationFrequency); }
        }
    }
}
