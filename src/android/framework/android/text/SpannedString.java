// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text and spans that do not change. */
public final class SpannedString implements Spanned, GetChars {
    private final String text;
    private final SpanSet spans;

    public SpannedString(CharSequence source) {
        text = source.toString();
        spans = new SpanSet();
        if (source instanceof Spanned s) copySpans(s, 0, source.length(), spans, 0);
    }

    static void copySpans(Spanned s, int start, int end, SpanSet into, int offset) {
        for (Object o : s.getSpans(start, end, Object.class)) {
            if (o instanceof NoCopySpan) continue;
            int st = Math.max(s.getSpanStart(o), start) - start + offset, en = Math.min(s.getSpanEnd(o), end) - start + offset;
            into.spans.add(o);
            into.ranges.add(new int[] {st, en, s.getSpanFlags(o)});
        }
    }

    public static SpannedString valueOf(CharSequence source) {
        return source instanceof SpannedString s ? s : new SpannedString(source);
    }

    @Override public int length() { return text.length(); }
    @Override public char charAt(int i) { return text.charAt(i); }
    @Override public CharSequence subSequence(int start, int end) {
        SpannableStringBuilder b = new SpannableStringBuilder(this, start, end);
        return new SpannedString(b);
    }
    @Override public String toString() { return text; }
    @Override public void getChars(int start, int end, char[] dest, int off) { text.getChars(start, end, dest, off); }
    @Override public <T> T[] getSpans(int start, int end, Class<T> type) { return spans.get(start, end, type); }
    @Override public int getSpanStart(Object tag) { return spans.start(tag); }
    @Override public int getSpanEnd(Object tag) { return spans.end(tag); }
    @Override public int getSpanFlags(Object tag) { return spans.flags(tag); }
    @Override public int nextSpanTransition(int start, int limit, Class type) { return spans.nextTransition(start, limit, type); }
    @Override public boolean equals(Object o) { return o instanceof Spanned s && s.toString().equals(text); }
    @Override public int hashCode() { return text.hashCode(); }
}
