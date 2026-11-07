// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** A ratio of two ints. */
public final class Rational extends Number implements Comparable<Rational> {
    private final int numerator, denominator;
    public Rational(int numerator, int denominator) { this.numerator = numerator; this.denominator = denominator; }
    public int getNumerator() { return numerator; }
    public int getDenominator() { return denominator; }
    @Override public double doubleValue() { return (double) numerator / denominator; }
    @Override public float floatValue() { return (float) numerator / denominator; }
    @Override public int intValue() { return denominator == 0 ? 0 : numerator / denominator; }
    @Override public long longValue() { return intValue(); }
    @Override public int compareTo(Rational o) { return Double.compare(doubleValue(), o.doubleValue()); }
}
