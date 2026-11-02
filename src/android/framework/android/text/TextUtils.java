// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

import java.util.Iterator;
import java.util.Locale;

/** Text helpers. */
public class TextUtils {
    public static final int CAP_MODE_CHARACTERS = 0x1000, CAP_MODE_WORDS = 0x2000, CAP_MODE_SENTENCES = 0x4000;
    public static final int SAFE_STRING_FLAG_TRIM = 0x1, SAFE_STRING_FLAG_SINGLE_LINE = 0x2,
        SAFE_STRING_FLAG_FIRST_LINE = 0x4;

    public enum TruncateAt { START, MIDDLE, END, MARQUEE, END_SMALL }

    private TextUtils() {}

    public static boolean isEmpty(CharSequence s) { return s == null || s.length() == 0; }
    public static boolean isGraphic(CharSequence s) {
        for (int i = 0; i < s.length(); ++i) if (!Character.isWhitespace(s.charAt(i)) && !Character.isISOControl(s.charAt(i))) return true;
        return false;
    }
    public static boolean isDigitsOnly(CharSequence s) {
        for (int i = 0; i < s.length(); ++i) if (!Character.isDigit(s.charAt(i))) return false;
        return true;
    }
    public static int getTrimmedLength(CharSequence s) {
        int start = 0, end = s.length();
        while (start < end && s.charAt(start) <= ' ') ++start;
        while (end > start && s.charAt(end - 1) <= ' ') --end;
        return end - start;
    }
    public static boolean equals(CharSequence a, CharSequence b) {
        if (a == b) return true;
        if (a == null || b == null || a.length() != b.length()) return false;
        if (a instanceof String && b instanceof String) return a.equals(b);
        for (int i = 0; i < a.length(); ++i) if (a.charAt(i) != b.charAt(i)) return false;
        return true;
    }
    public static String join(CharSequence delimiter, Object[] tokens) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < tokens.length; ++i) b.append(i > 0 ? delimiter : "").append(tokens[i]);
        return b.toString();
    }
    public static String join(CharSequence delimiter, Iterable<?> tokens) {
        StringBuilder b = new StringBuilder();
        Iterator<?> it = tokens.iterator();
        while (it.hasNext()) { b.append(it.next()); if (it.hasNext()) b.append(delimiter); }
        return b.toString();
    }
    public static String[] split(String text, String expression) {
        return text.isEmpty() ? new String[0] : text.split(expression, -1);
    }
    public static CharSequence concat(CharSequence... text) {
        StringBuilder b = new StringBuilder();
        for (CharSequence t : text) b.append(t);
        return b.toString();
    }
    public static String substring(CharSequence source, int start, int end) { return source.subSequence(start, end).toString(); }
    public static void getChars(CharSequence s, int start, int end, char[] dest, int destoff) {
        if (s instanceof String str) { str.getChars(start, end, dest, destoff); return; }
        if (s instanceof GetChars g) { g.getChars(start, end, dest, destoff); return; }
        for (int i = start; i < end; ++i) dest[destoff++] = s.charAt(i);
    }
    public static int indexOf(CharSequence s, char ch) { return indexOf(s, ch, 0); }
    public static int indexOf(CharSequence s, char ch, int start) {
        for (int i = start; i < s.length(); ++i) if (s.charAt(i) == ch) return i;
        return -1;
    }
    public static int lastIndexOf(CharSequence s, char ch) {
        for (int i = s.length() - 1; i >= 0; --i) if (s.charAt(i) == ch) return i;
        return -1;
    }
    public static CharSequence stringOrSpannedString(CharSequence source) {
        if (source == null) return null;
        if (source instanceof SpannedString) return source;
        if (source instanceof Spanned) return new SpannedString(source);
        return source.toString();
    }
    public static String htmlEncode(String s) {
        StringBuilder b = new StringBuilder();
        for (char c : s.toCharArray()) {
            switch (c) {
                case '<' -> b.append("&lt;");
                case '>' -> b.append("&gt;");
                case '&' -> b.append("&amp;");
                case '\'' -> b.append("&#39;");
                case '"' -> b.append("&quot;");
                default -> b.append(c);
            }
        }
        return b.toString();
    }
    public static int getLayoutDirectionFromLocale(Locale locale) {
        if (locale == null) return 0;
        byte d = Character.getDirectionality(Character.codePointAt(locale.getDisplayName(locale) + " ", 0));
        String lang = locale.getLanguage();
        return lang.equals("ar") || lang.equals("fa") || lang.equals("he") || lang.equals("iw") || lang.equals("ur")
            || d == Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC ? 1 : 0;
    }
    public static CharSequence ellipsize(CharSequence text, TextPaint p, float avail, TruncateAt where) {
        return ellipsize(text, p, avail, where, false, null);
    }
    public static CharSequence ellipsize(CharSequence text, TextPaint paint, float avail, TruncateAt where,
                                         boolean preserveLength, EllipsizeCallback callback) {
        if (paint.measureText(text, 0, text.length()) <= avail) return text;
        String e = "…";
        int end = text.length();
        while (end > 0 && paint.measureText(text, 0, end) + paint.measureText(e) > avail) --end;
        return text.subSequence(0, end) + e;
    }
    public interface EllipsizeCallback {
        void ellipsized(int start, int end);
    }
    public static int getCapsMode(CharSequence cs, int off, int reqModes) { return 0; }
    public static final android.os.Parcelable.Creator<CharSequence> CHAR_SEQUENCE_CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public CharSequence createFromParcel(android.os.Parcel p) { return (CharSequence) p.readValue(null); }
        @Override public CharSequence[] newArray(int size) { return new CharSequence[size]; }
    };
    public static void writeToParcel(CharSequence cs, android.os.Parcel p, int parcelableFlags) { p.writeValue(cs); }
    public static String nullIfEmpty(String str) { return isEmpty(str) ? null : str; }
    public static String emptyIfNull(String str) { return str == null ? "" : str; }
    public static CharSequence trimToSize(CharSequence text, int size) {
        return text == null || text.length() <= size ? text : text.subSequence(0, size);
    }
}
