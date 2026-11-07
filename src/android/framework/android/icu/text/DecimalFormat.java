// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.icu.text;

/** Decimal number formats, on java.text's. */
public class DecimalFormat extends NumberFormat {
    private final java.text.NumberFormat format;
    DecimalFormat(java.text.NumberFormat format) { this.format = format; }
    public DecimalFormat() { format = new java.text.DecimalFormat(); }
    public DecimalFormat(String pattern) { format = new java.text.DecimalFormat(pattern); }
    public DecimalFormat(String pattern, DecimalFormatSymbols symbols) { format = new java.text.DecimalFormat(pattern, symbols.symbols); }
    @Override public String format(double number) { return format.format(number); }
    @Override public String format(long number) { return format.format(number); }
    public String format(java.math.BigDecimal number) { return format.format(number); }
    public int getGroupingSize() { return format instanceof java.text.DecimalFormat d ? d.getGroupingSize() : 3; }
    public void setGroupingSize(int newValue) { if (format instanceof java.text.DecimalFormat d) d.setGroupingSize(newValue); }
    @Override public Number parse(String text) throws java.text.ParseException { return format.parse(text); }
    @Override public void setMaximumFractionDigits(int newValue) { format.setMaximumFractionDigits(newValue); }
    @Override public void setMinimumFractionDigits(int newValue) { format.setMinimumFractionDigits(newValue); }
    @Override public void setGroupingUsed(boolean newValue) { format.setGroupingUsed(newValue); }
    public void setMaximumSignificantDigits(int max) {}
    public void setMinimumSignificantDigits(int min) {}
    public void setSignificantDigitsUsed(boolean useSignificantDigits) {}
    public void setRoundingMode(int roundingMode) { if (format instanceof java.text.DecimalFormat d) d.setRoundingMode(java.math.RoundingMode.valueOf(roundingMode)); }
    public void setDecimalFormatSymbols(DecimalFormatSymbols newSymbols) { if (format instanceof java.text.DecimalFormat d) d.setDecimalFormatSymbols(newSymbols.symbols); }
    public DecimalFormatSymbols getDecimalFormatSymbols() { return new DecimalFormatSymbols(); }
    public void applyPattern(String pattern) { if (format instanceof java.text.DecimalFormat d) d.applyPattern(pattern); }
    public String toPattern() { return format instanceof java.text.DecimalFormat d ? d.toPattern() : ""; }
}
