// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text that does not change, with spans that can. */
public class SpannableString implements Spannable, GetChars {
    private final String text;
    private final SpanSet spans = new SpanSet();

    public SpannableString(CharSequence source) {
        text = source.toString();
        if (source instanceof Spanned s) SpannedString.copySpans(s, 0, source.length(), spans, 0);
    }

    public SpannableString(CharSequence source, boolean ignoreNoCopySpan) { this(source); }

    public static SpannableString valueOf(CharSequence source) {
        return source instanceof SpannableString s ? s : new SpannableString(source);
    }

    @Override public int length() { return text.length(); }
    @Override public char charAt(int i) { return text.charAt(i); }
    @Override public CharSequence subSequence(int start, int end) {
        return new SpannableString(new SpannableStringBuilder(this, start, end));
    }
    @Override public String toString() { return text; }
    @Override public void getChars(int start, int end, char[] dest, int off) { text.getChars(start, end, dest, off); }
    @Override public void setSpan(Object what, int start, int end, int flags) { spans.set(what, start, end, flags, text.length()); }
    @Override public void removeSpan(Object what) { spans.remove(what); }
    @Override public <T> T[] getSpans(int start, int end, Class<T> type) { return spans.get(start, end, type); }
    @Override public int getSpanStart(Object tag) { return spans.start(tag); }
    @Override public int getSpanEnd(Object tag) { return spans.end(tag); }
    @Override public int getSpanFlags(Object tag) { return spans.flags(tag); }
    @Override public int nextSpanTransition(int start, int limit, Class type) { return spans.nextTransition(start, limit, type); }
    @Override public boolean equals(Object o) { return o instanceof Spanned s && s.toString().equals(text); }
    @Override public int hashCode() { return text.hashCode(); }
}
