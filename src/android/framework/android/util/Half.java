// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** An IEEE 754 binary16 value: 1 sign bit, 5 exponent bits, 10 mantissa bits. */
public final class Half extends Number implements Comparable<Half> {
    public static final int SIZE = 16;
    public static final short EPSILON = 0x1400, LOWEST_VALUE = (short) 0xfbff, MAX_VALUE = 0x7bff, MIN_NORMAL = 0x0400,
        MIN_VALUE = 0x0001, NaN = 0x7e00, NEGATIVE_INFINITY = (short) 0xfc00, NEGATIVE_ZERO = (short) 0x8000,
        POSITIVE_INFINITY = 0x7c00, POSITIVE_ZERO = 0x0000;
    public static final int MAX_EXPONENT = 15, MIN_EXPONENT = -14;

    private final short value;

    public Half(short value) { this.value = value; }
    public Half(float value) { this.value = toHalf(value); }
    public Half(double value) { this.value = toHalf((float) value); }
    public Half(String s) throws NumberFormatException { value = toHalf(Float.parseFloat(s)); }

    public static Half valueOf(short h) { return new Half(h); }
    public static Half valueOf(float f) { return new Half(f); }
    public static Half valueOf(String s) { return new Half(s); }

    public short halfValue() { return value; }
    @Override public byte byteValue() { return (byte) toFloat(value); }
    @Override public short shortValue() { return (short) toFloat(value); }
    @Override public int intValue() { return (int) toFloat(value); }
    @Override public long longValue() { return (long) toFloat(value); }
    @Override public float floatValue() { return toFloat(value); }
    @Override public double doubleValue() { return toFloat(value); }
    public boolean isNaN() { return isNaN(value); }

    public static boolean isNaN(short h) { return (h & 0x7fff) > 0x7c00; }
    public static boolean isInfinite(short h) { return (h & 0x7fff) == 0x7c00; }
    public static int halfToIntBits(short h) { return isNaN(h) ? NaN : h & 0xffff; }
    public static int halfToRawIntBits(short h) { return h & 0xffff; }
    public static short intBitsToHalf(int bits) { return (short) bits; }

    /** The float of a half, exactly: each half value is a float value. */
    public static float toFloat(short h) {
        int bits = h & 0xffff;
        int sign = (bits & 0x8000) << 16;
        int exp = (bits >>> 10) & 0x1f;
        int mant = bits & 0x3ff;
        if (exp == 0) {
            float f = Math.scalb((float) mant, -24);
            return sign != 0 ? -f : f;
        }
        if (exp == 0x1f) return Float.intBitsToFloat(sign | 0x7f800000 | (mant << 13));
        return Float.intBitsToFloat(sign | ((exp + 112) << 23) | (mant << 13));
    }

    /** The nearest half of a float, ties to even. A NaN stays a NaN, and a value too large becomes infinity. */
    public static short toHalf(float f) {
        int bits = Float.floatToRawIntBits(f);
        int sign = (bits >>> 16) & 0x8000;
        int exp = (bits >>> 23) & 0xff;
        int mant = bits & 0x7fffff;
        if (exp == 0xff) return (short) (sign | 0x7c00 | (mant != 0 ? 0x200 : 0));
        int e = exp - 112;
        if (e >= 0x1f) return (short) (sign | 0x7c00);
        if (e <= 0) {
            if (e < -10) return (short) sign;
            mant |= 0x800000;
            int shift = 14 - e;
            int half = mant >> shift;
            int rem = mant & ((1 << shift) - 1);
            int mid = 1 << (shift - 1);
            if (rem > mid || (rem == mid && (half & 1) != 0)) ++half;
            return (short) (sign | half);
        }
        int half = (e << 10) | (mant >> 13);
        int rem = mant & 0x1fff;
        if (rem > 0x1000 || (rem == 0x1000 && (half & 1) != 0)) ++half;
        return (short) (sign | half);
    }

    public static String toString(short h) { return Float.toString(toFloat(h)); }

    /** As Float.compare: -0 is less than +0, and NaN is greater than each other value. */
    public static int compare(short x, short y) { return Float.compare(toFloat(x), toFloat(y)); }

    @Override public int compareTo(Half h) { return compare(value, h.value); }
    @Override public boolean equals(Object o) { return o instanceof Half h && halfToIntBits(h.value) == halfToIntBits(value); }
    @Override public int hashCode() { return halfToIntBits(value); }
    @Override public String toString() { return toString(value); }
}
