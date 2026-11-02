// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text and spans that both change: the edits move the spans by their flags. */
public class SpannableStringBuilder implements CharSequence, GetChars, Spannable, Editable, Appendable {
    private final StringBuilder text = new StringBuilder();
    private SpanSet spans = new SpanSet();
    private InputFilter[] filters = new InputFilter[0];

    public SpannableStringBuilder() {}
    public SpannableStringBuilder(CharSequence text) { this(text, 0, text.length()); }
    public SpannableStringBuilder(CharSequence text, int start, int end) {
        this.text.append(text, start, end);
        if (text instanceof Spanned s) SpannedString.copySpans(s, start, end, spans, 0);
    }

    public static SpannableStringBuilder valueOf(CharSequence source) {
        return source instanceof SpannableStringBuilder s ? s : new SpannableStringBuilder(source);
    }

    @Override public int length() { return text.length(); }
    @Override public char charAt(int i) { return text.charAt(i); }
    @Override public CharSequence subSequence(int start, int end) { return new SpannableStringBuilder(this, start, end); }
    @Override public String toString() { return text.toString(); }
    @Override public void getChars(int start, int end, char[] dest, int off) { text.getChars(start, end, dest, off); }

    @Override
    public SpannableStringBuilder replace(int start, int end, CharSequence tb, int tbstart, int tbend) {
        for (InputFilter f : filters) {
            CharSequence r = f.filter(tb, tbstart, tbend, this, start, end);
            if (r != null) { tb = r; tbstart = 0; tbend = r.length(); }
        }
        TextWatcher[] watchers = getSpans(start, start + (end - start), TextWatcher.class);
        int count = tbend - tbstart;
        for (TextWatcher w : watchers) w.beforeTextChanged(this, start, end - start, count);
        text.replace(start, end, tb.subSequence(tbstart, tbend).toString());
        spans.replace(start, end, count);
        if (tb instanceof Spanned s) SpannedString.copySpans(s, tbstart, tbend, spans, start);
        for (TextWatcher w : watchers) w.onTextChanged(this, start, end - start, count);
        for (TextWatcher w : watchers) w.afterTextChanged(this);
        return this;
    }
    @Override public SpannableStringBuilder replace(int start, int end, CharSequence tb) { return replace(start, end, tb, 0, tb.length()); }
    @Override public SpannableStringBuilder insert(int where, CharSequence tb, int start, int end) { return replace(where, where, tb, start, end); }
    @Override public SpannableStringBuilder insert(int where, CharSequence tb) { return replace(where, where, tb, 0, tb.length()); }
    @Override public SpannableStringBuilder delete(int start, int end) { return replace(start, end, "", 0, 0); }
    @Override public SpannableStringBuilder append(CharSequence t) { int n = length(); return replace(n, n, t, 0, t.length()); }
    public SpannableStringBuilder append(CharSequence t, Object what, int flags) {
        int start = length();
        append(t);
        setSpan(what, start, length(), flags);
        return this;
    }
    @Override public SpannableStringBuilder append(CharSequence t, int start, int end) { int n = length(); return replace(n, n, t, start, end); }
    @Override public SpannableStringBuilder append(char t) { return append(String.valueOf(t)); }
    @Override public void clear() { replace(0, length(), "", 0, 0); }
    @Override public void clearSpans() { spans = new SpanSet(); }
    @Override public void setFilters(InputFilter[] filters) { this.filters = filters; }
    @Override public InputFilter[] getFilters() { return filters; }
    @Override public void setSpan(Object what, int start, int end, int flags) { spans.set(what, start, end, flags, text.length()); }
    @Override public void removeSpan(Object what) { spans.remove(what); }
    @Override public <T> T[] getSpans(int start, int end, Class<T> type) { return spans.get(start, end, type); }
    public <T> T[] getSpans(int start, int end, Class<T> type, boolean sortByInsertionOrder) { return spans.get(start, end, type); }
    @Override public int getSpanStart(Object tag) { return spans.start(tag); }
    @Override public int getSpanEnd(Object tag) { return spans.end(tag); }
    @Override public int getSpanFlags(Object tag) { return spans.flags(tag); }
    @Override public int nextSpanTransition(int start, int limit, Class type) { return spans.nextTransition(start, limit, type); }
    public int getTextWatcherDepth() { return 0; }
    @Override public boolean equals(Object o) { return o instanceof Spanned s && s.toString().equals(toString()); }
    @Override public int hashCode() { return toString().hashCode(); }
}
