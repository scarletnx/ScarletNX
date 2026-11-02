// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.icu.text;

/** Number formats, on java.text. */
public abstract class NumberFormat {
    public static NumberFormat getInstance(java.util.Locale locale) { return new DecimalFormat(java.text.NumberFormat.getInstance(locale)); }
    public static NumberFormat getInstance() { return getInstance(java.util.Locale.getDefault()); }
    public abstract String format(double number);
    public abstract String format(long number);
    public abstract Number parse(String text) throws java.text.ParseException;
    public abstract void setMaximumFractionDigits(int newValue);
    public abstract void setMinimumFractionDigits(int newValue);
    public abstract void setGroupingUsed(boolean newValue);
}
