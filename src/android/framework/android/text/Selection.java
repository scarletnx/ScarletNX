// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** The cursor and the selection of a text, kept as two spans on it. */
public class Selection {
    public static final Object SELECTION_START = new NoCopySpan.Concrete();
    public static final Object SELECTION_END = new NoCopySpan.Concrete();

    private Selection() {}

    public static final int getSelectionStart(CharSequence text) { return text instanceof Spanned s ? s.getSpanStart(SELECTION_START) : -1; }
    public static final int getSelectionEnd(CharSequence text) { return text instanceof Spanned s ? s.getSpanStart(SELECTION_END) : -1; }
    public static void setSelection(Spannable text, int start, int stop) {
        if (getSelectionStart(text) != start || getSelectionEnd(text) != stop) {
            text.setSpan(SELECTION_START, start, start, Spanned.SPAN_POINT_POINT | Spanned.SPAN_INTERMEDIATE);
            text.setSpan(SELECTION_END, stop, stop, Spanned.SPAN_POINT_POINT);
        }
    }
    public static final void setSelection(Spannable text, int index) { setSelection(text, index, index); }
    public static final void selectAll(Spannable text) { setSelection(text, 0, text.length()); }
    public static final void extendSelection(Spannable text, int index) {
        if (text.getSpanStart(SELECTION_END) != index) text.setSpan(SELECTION_END, index, index, Spanned.SPAN_POINT_POINT);
    }
    public static final void removeSelection(Spannable text) {
        text.removeSpan(SELECTION_START);
        text.removeSpan(SELECTION_END);
    }
}
