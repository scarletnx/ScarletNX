// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text measured ahead of its layout (here, measured with its layout). */
public class PrecomputedText implements Spannable {
    private final SpannableString text;

    private PrecomputedText(CharSequence text) { this.text = new SpannableString(text); }
    public static PrecomputedText create(CharSequence text, Params params) { return new PrecomputedText(text); }
    public Params getParams() { return null; }
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

        Params(TextPaint paint) { this.paint = paint; }
        public TextPaint getTextPaint() { return paint; }

        public static class Builder {
            private final TextPaint paint;

            public Builder(TextPaint paint) { this.paint = paint; }
            public Builder setBreakStrategy(int strategy) { return this; }
            public Builder setHyphenationFrequency(int frequency) { return this; }
            public Builder setTextDirection(TextDirectionHeuristic textDir) { return this; }
            public Params build() { return new Params(paint); }
        }
    }
}
