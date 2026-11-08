// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.cookie;

import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

public final class DateUtils {
    public static final String PATTERN_RFC1123 = "EEE, dd MMM yyyy HH:mm:ss zzz",
        PATTERN_RFC1036 = "EEEE, dd-MMM-yy HH:mm:ss zzz", PATTERN_ASCTIME = "EEE MMM d HH:mm:ss yyyy";
    public static final TimeZone GMT = TimeZone.getTimeZone("GMT");
    private static final String[] DEFAULT_PATTERNS = {PATTERN_RFC1036, PATTERN_RFC1123, PATTERN_ASCTIME};

    private DateUtils() {}
    public static Date parseDate(String dateValue) throws DateParseException { return parseDate(dateValue, null, null); }
    public static Date parseDate(String dateValue, String[] dateFormats) throws DateParseException {
        return parseDate(dateValue, dateFormats, null);
    }
    public static Date parseDate(String dateValue, String[] dateFormats, Date startDate) throws DateParseException {
        if (dateValue == null) throw new IllegalArgumentException("dateValue is null");
        String v = dateValue;
        if (v.length() > 1 && v.startsWith("'") && v.endsWith("'")) v = v.substring(1, v.length() - 1);
        for (String pattern : dateFormats != null ? dateFormats : DEFAULT_PATTERNS) {
            SimpleDateFormat format = new SimpleDateFormat(pattern, Locale.US);
            format.setTimeZone(GMT);
            try {
                return format.parse(v);
            } catch (ParseException e) {
                // the next pattern
            }
        }
        throw new DateParseException("Unable to parse the date " + dateValue);
    }
    public static String formatDate(Date date) { return formatDate(date, PATTERN_RFC1123); }
    public static String formatDate(Date date, String pattern) {
        SimpleDateFormat format = new SimpleDateFormat(pattern, Locale.US);
        format.setTimeZone(GMT);
        return format.format(date);
    }
}
