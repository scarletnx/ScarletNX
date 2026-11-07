// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text that can change. */
public interface Editable extends CharSequence, GetChars, Spannable, Appendable {
    Editable replace(int st, int en, CharSequence source, int start, int end);
    Editable replace(int st, int en, CharSequence text);
    Editable insert(int where, CharSequence text, int start, int end);
    Editable insert(int where, CharSequence text);
    Editable delete(int st, int en);
    @Override Editable append(CharSequence text);
    @Override Editable append(CharSequence text, int start, int end);
    @Override Editable append(char text);
    void clear();
    void clearSpans();
    void setFilters(InputFilter[] filters);
    InputFilter[] getFilters();

    class Factory {
        private static final Factory INSTANCE = new Factory();

        public static Factory getInstance() { return INSTANCE; }

        public Editable newEditable(CharSequence source) { return new SpannableStringBuilder(source); }
    }
}
