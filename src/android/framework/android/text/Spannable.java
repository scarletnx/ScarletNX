// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Text whose spans can change. */
public interface Spannable extends Spanned {
    void setSpan(Object what, int start, int end, int flags);

    void removeSpan(Object what);

    default void removeSpan(Object what, int flags) { removeSpan(what); }

    class Factory {
        private static final Factory INSTANCE = new Factory();

        public static Factory getInstance() { return INSTANCE; }

        public Spannable newSpannable(CharSequence source) { return new SpannableString(source); }
    }
}
