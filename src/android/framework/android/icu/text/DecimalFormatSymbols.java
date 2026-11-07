// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.icu.text;

/** The symbols of decimal numbers in a locale. */
public class DecimalFormatSymbols {
    final java.text.DecimalFormatSymbols symbols;
    public DecimalFormatSymbols() { symbols = new java.text.DecimalFormatSymbols(); }
    public DecimalFormatSymbols(java.util.Locale locale) { symbols = new java.text.DecimalFormatSymbols(locale); }
    public static DecimalFormatSymbols getInstance(java.util.Locale locale) { return new DecimalFormatSymbols(locale); }
    public char getDecimalSeparator() { return symbols.getDecimalSeparator(); }
    public char getGroupingSeparator() { return symbols.getGroupingSeparator(); }
    public char getMinusSign() { return symbols.getMinusSign(); }
    public char getZeroDigit() { return symbols.getZeroDigit(); }
    public String[] getDigitStrings() {
        String[] digits = new String[10];
        for (int i = 0; i < 10; i++) digits[i] = String.valueOf((char) (symbols.getZeroDigit() + i));
        return digits;
    }
    public String getExponentSeparator() { return symbols.getExponentSeparator(); }
    public void setDecimalSeparator(char c) { symbols.setDecimalSeparator(c); }
    public void setGroupingSeparator(char c) { symbols.setGroupingSeparator(c); }
    public void setZeroDigit(char c) { symbols.setZeroDigit(c); }
}
