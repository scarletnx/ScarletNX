// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.text.util;

import android.text.Spannable;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.method.LinkMovementMethod;
import android.text.method.MovementMethod;
import android.text.style.URLSpan;
import android.widget.TextView;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Finds web addresses, e-mail addresses and phone numbers in a text, and makes them links (URLSpans). */
public class Linkify {
    public static final int WEB_URLS = 0x01, EMAIL_ADDRESSES = 0x02, PHONE_NUMBERS = 0x04, MAP_ADDRESSES = 0x08, ALL = 0x0f;

    private static final Pattern WEB = Pattern.compile(
        "((?:(?:https?|rtsp|ftp)://)?(?:[\\w-]+(?::[^@\\s]*)?@)?(?:(?:[A-Za-z0-9\\u00a0-\\uffff](?:[A-Za-z0-9\\u00a0-\\uffff-]{0,62}[A-Za-z0-9\\u00a0-\\uffff])?\\.)+[A-Za-z\\u00a0-\\uffff]{2,63}|(?:\\d{1,3}\\.){3}\\d{1,3})(?::\\d{1,5})?(?:[/?#][^\\s]*)?)",
        Pattern.CASE_INSENSITIVE);
    private static final Pattern EMAIL = Pattern.compile("[A-Za-z0-9+._%-]{1,256}@[A-Za-z0-9][A-Za-z0-9-]{0,64}(?:\\.[A-Za-z0-9][A-Za-z0-9-]{0,25})+");
    private static final Pattern PHONE = Pattern.compile("(?:\\+[0-9]+[\\- .]*)?(?:\\([0-9]+\\)[\\- .]*)?[0-9][0-9\\- .]{3,}[0-9]");

    public interface MatchFilter {
        boolean acceptMatch(CharSequence s, int start, int end);
    }

    public interface TransformFilter {
        String transformUrl(Matcher match, String url);
    }

    /** Accepts a web address unless an '@' comes just before it (then it is part of an e-mail address). */
    public static final MatchFilter sUrlMatchFilter = (s, start, end) -> start == 0 || s.charAt(start - 1) != '@';
    /** Accepts a phone number of at least 5 digits. */
    public static final MatchFilter sPhoneNumberMatchFilter = (s, start, end) -> {
        int digits = 0;
        for (int i = start; i < end; ++i) if (Character.isDigit(s.charAt(i)) && ++digits >= 5) return true;
        return false;
    };
    /** Keeps only the digits and the '+' of a phone number. */
    public static final TransformFilter sPhoneNumberTransformFilter = (match, url) -> url.replaceAll("[^0-9+]", "");

    public Linkify() {}

    public static final boolean addLinks(Spannable text, int mask) {
        if (mask == 0) return false;
        for (URLSpan old : text.getSpans(0, text.length(), URLSpan.class)) text.removeSpan(old);
        boolean found = false;
        if ((mask & WEB_URLS) != 0)
            found |= addLinks(text, WEB, "http://", new String[] {"https://", "rtsp://", "ftp://"}, sUrlMatchFilter, null);
        if ((mask & EMAIL_ADDRESSES) != 0) found |= addLinks(text, EMAIL, "mailto:", null, null, null);
        if ((mask & PHONE_NUMBERS) != 0)
            found |= addLinks(text, PHONE, "tel:", null, sPhoneNumberMatchFilter, sPhoneNumberTransformFilter);
        /* MAP_ADDRESSES: no address finder here, as on Android 9 and later without the WebView's */
        return found;
    }
    public static final boolean addLinks(TextView text, int mask) {
        if (mask == 0) return false;
        CharSequence t = text.getText();
        if (t instanceof Spannable s) {
            if (!addLinks(s, mask)) return false;
            addLinkMovementMethod(text);
            return true;
        }
        SpannableString s = SpannableString.valueOf(t);
        if (!addLinks(s, mask)) return false;
        addLinkMovementMethod(text);
        text.setText(s);
        return true;
    }
    private static void addLinkMovementMethod(TextView t) {
        MovementMethod m = t.getMovementMethod();
        if (!(m instanceof LinkMovementMethod)) t.setMovementMethod(LinkMovementMethod.getInstance());
    }

    public static final void addLinks(TextView text, Pattern pattern, String scheme) { addLinks(text, pattern, scheme, null, null, null); }
    public static final void addLinks(TextView text, Pattern pattern, String scheme, MatchFilter matchFilter, TransformFilter transformFilter) {
        addLinks(text, pattern, scheme, null, matchFilter, transformFilter);
    }
    public static final void addLinks(TextView text, Pattern pattern, String defaultScheme, String[] schemes,
                                      MatchFilter matchFilter, TransformFilter transformFilter) {
        SpannableString s = SpannableString.valueOf(text.getText());
        if (addLinks(s, pattern, defaultScheme, schemes, matchFilter, transformFilter)) {
            text.setText(s);
            addLinkMovementMethod(text);
        }
    }
    public static final boolean addLinks(Spannable text, Pattern pattern, String scheme) { return addLinks(text, pattern, scheme, null, null, null); }
    public static final boolean addLinks(Spannable spannable, Pattern pattern, String scheme, MatchFilter matchFilter, TransformFilter transformFilter) {
        return addLinks(spannable, pattern, scheme, null, matchFilter, transformFilter);
    }
    public static final boolean addLinks(Spannable spannable, Pattern pattern, String defaultScheme, String[] schemes,
                                         MatchFilter matchFilter, TransformFilter transformFilter) {
        if (spannable == null || pattern == null) throw new IllegalArgumentException("spannable and pattern must not be null");
        String first = defaultScheme == null ? "" : defaultScheme.toLowerCase(Locale.ROOT);
        String[] all = new String[(schemes != null ? schemes.length : 0) + 1];
        all[0] = first;
        for (int i = 1; i < all.length; ++i) all[i] = schemes[i - 1] == null ? "" : schemes[i - 1].toLowerCase(Locale.ROOT);
        boolean found = false;
        Matcher m = pattern.matcher(spannable);
        while (m.find()) {
            int start = m.start(), end = m.end();
            if (matchFilter != null && !matchFilter.acceptMatch(spannable, start, end)) continue;
            spannable.setSpan(new URLSpan(makeUrl(m.group(0), all, m, transformFilter)), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            found = true;
        }
        return found;
    }
    private static String makeUrl(String url, String[] schemes, Matcher m, TransformFilter filter) {
        if (filter != null) url = filter.transformUrl(m, url);
        for (String scheme : schemes) {
            if (!scheme.isEmpty() && url.regionMatches(true, 0, scheme, 0, scheme.length()))
                return url.startsWith(scheme) ? url : scheme + url.substring(scheme.length());
        }
        return schemes[0] + url;
    }
}
