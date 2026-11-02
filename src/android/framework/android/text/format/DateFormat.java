// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.format;

import java.util.Calendar;
import java.util.Date;

/** Date formats in Android's pattern letters, which are java.text's. */
public class DateFormat {
    public static CharSequence format(CharSequence inFormat, Date inDate) {
        return new java.text.SimpleDateFormat(inFormat.toString()).format(inDate);
    }
    public static CharSequence format(CharSequence inFormat, long inTimeInMillis) { return format(inFormat, new Date(inTimeInMillis)); }
    public static CharSequence format(CharSequence inFormat, Calendar inDate) { return format(inFormat, inDate.getTime()); }
    public static boolean is24HourFormat(android.content.Context context) { return true; }
    public static java.text.DateFormat getDateFormat(android.content.Context context) { return java.text.DateFormat.getDateInstance(java.text.DateFormat.SHORT); }
    public static java.text.DateFormat getTimeFormat(android.content.Context context) { return java.text.DateFormat.getTimeInstance(java.text.DateFormat.SHORT); }
    public static String getBestDateTimePattern(java.util.Locale locale, String skeleton) { return skeleton; }
}
