// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.format;

import android.content.Context;
import java.util.Locale;

/** Sizes of files, in units of 1000 bytes. */
public final class Formatter {
    private static final String[] UNITS = {"B", "kB", "MB", "GB", "TB", "PB"};

    public Formatter() {}

    public static String formatFileSize(Context context, long sizeBytes) { return format(context, sizeBytes, false); }
    public static String formatShortFileSize(Context context, long sizeBytes) { return format(context, sizeBytes, true); }
    @Deprecated public static String formatIpAddress(int ipv4Address) {
        return (ipv4Address & 0xff) + "." + ((ipv4Address >> 8) & 0xff) + "." + ((ipv4Address >> 16) & 0xff) + "." + ((ipv4Address >>> 24) & 0xff);
    }

    private static String format(Context context, long sizeBytes, boolean shorter) {
        if (context == null) return "";
        boolean negative = sizeBytes < 0;
        double result = negative ? -(double) sizeBytes : sizeBytes;
        int unit = 0;
        while (result > 900 && unit < UNITS.length - 1) {
            result /= 1000;
            ++unit;
        }
        String number;
        if (unit == 0 || result >= 100) number = String.format(Locale.getDefault(), "%.0f", result);
        else if (result < 1) number = String.format(Locale.getDefault(), "%.2f", result);
        else if (result < 10) number = String.format(Locale.getDefault(), shorter ? "%.1f" : "%.2f", result);
        else number = String.format(Locale.getDefault(), shorter ? "%.0f" : "%.2f", result);
        return (negative ? "-" : "") + number + " " + UNITS[unit];
    }
}
