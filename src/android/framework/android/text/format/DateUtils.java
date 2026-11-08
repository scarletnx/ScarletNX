// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.format;

/** Time spans as words. */
public class DateUtils {
    public static final long SECOND_IN_MILLIS = 1000, MINUTE_IN_MILLIS = 60000, HOUR_IN_MILLIS = 3600000,
        DAY_IN_MILLIS = 86400000, WEEK_IN_MILLIS = 604800000;

    public static boolean isToday(long when) {
        java.time.ZoneId z = java.time.ZoneId.systemDefault();
        return java.time.Instant.ofEpochMilli(when).atZone(z).toLocalDate().equals(java.time.LocalDate.now(z));
    }

    public static CharSequence getRelativeTimeSpanString(long time, long now, long minResolution) {
        long d = now - time, a = Math.abs(d);
        String unit;
        long n;
        if (a < MINUTE_IN_MILLIS && minResolution < MINUTE_IN_MILLIS) { n = a / SECOND_IN_MILLIS; unit = n == 1 ? "second" : "seconds"; }
        else if (a < HOUR_IN_MILLIS && minResolution < HOUR_IN_MILLIS) { n = a / MINUTE_IN_MILLIS; unit = n == 1 ? "minute" : "minutes"; }
        else if (a < DAY_IN_MILLIS && minResolution < DAY_IN_MILLIS) { n = a / HOUR_IN_MILLIS; unit = n == 1 ? "hour" : "hours"; }
        else if (a < WEEK_IN_MILLIS) { n = a / DAY_IN_MILLIS; if (n == 1) return d >= 0 ? "Yesterday" : "Tomorrow"; unit = "days"; }
        else return java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM).format(new java.util.Date(time));
        return d >= 0 ? n + " " + unit + " ago" : "In " + n + " " + unit;
    }

    public static CharSequence getRelativeTimeSpanString(long startTime) {
        return getRelativeTimeSpanString(startTime, System.currentTimeMillis(), MINUTE_IN_MILLIS);
    }

    public static String formatElapsedTime(long elapsedSeconds) {
        long h = elapsedSeconds / 3600, m = elapsedSeconds / 60 % 60, s = elapsedSeconds % 60;
        return h > 0 ? String.format("%d:%02d:%02d", h, m, s) : String.format("%02d:%02d", m, s);
    }
}
