// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.text;

/** A filter of the text an edit puts in. */
public interface InputFilter {
    CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend);

    /** Keeps the text at most a number of chars long, and does not split a surrogate pair. */
    class LengthFilter implements InputFilter {
        private final int max;

        public LengthFilter(int max) { this.max = max; }
        public int getMax() { return max; }
        @Override public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
            int keep = max - (dest.length() - (dend - dstart));
            if (keep <= 0) return "";
            if (keep >= end - start) return null;
            keep += start;
            if (Character.isHighSurrogate(source.charAt(keep - 1)) && --keep == start) return "";
            return source.subSequence(start, keep);
        }
    }

    /** Makes the text upper case. */
    class AllCaps implements InputFilter {
        private final java.util.Locale locale;

        public AllCaps() { locale = null; }
        public AllCaps(java.util.Locale locale) { this.locale = locale; }
        @Override public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
            String in = source.subSequence(start, end).toString();
            String up = in.toUpperCase(locale != null ? locale : java.util.Locale.getDefault());
            return up.equals(in) ? null : up;
        }
    }
}
