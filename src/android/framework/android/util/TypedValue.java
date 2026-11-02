// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** A typed resource value: Res_value of libandroidfw, with its resource and string. */
public class TypedValue {
    public static final int TYPE_NULL = 0x00;
    public static final int TYPE_REFERENCE = 0x01;
    public static final int TYPE_ATTRIBUTE = 0x02;
    public static final int TYPE_STRING = 0x03;
    public static final int TYPE_FLOAT = 0x04;
    public static final int TYPE_DIMENSION = 0x05;
    public static final int TYPE_FRACTION = 0x06;
    public static final int TYPE_FIRST_INT = 0x10;
    public static final int TYPE_INT_DEC = 0x10;
    public static final int TYPE_INT_HEX = 0x11;
    public static final int TYPE_INT_BOOLEAN = 0x12;
    public static final int TYPE_FIRST_COLOR_INT = 0x1c;
    public static final int TYPE_INT_COLOR_ARGB8 = 0x1c;
    public static final int TYPE_INT_COLOR_RGB8 = 0x1d;
    public static final int TYPE_INT_COLOR_ARGB4 = 0x1e;
    public static final int TYPE_INT_COLOR_RGB4 = 0x1f;
    public static final int TYPE_LAST_COLOR_INT = 0x1f;
    public static final int TYPE_LAST_INT = 0x1f;

    public static final int COMPLEX_UNIT_SHIFT = 0;
    public static final int COMPLEX_UNIT_MASK = 0xf;
    public static final int COMPLEX_UNIT_PX = 0;
    public static final int COMPLEX_UNIT_DIP = 1;
    public static final int COMPLEX_UNIT_SP = 2;
    public static final int COMPLEX_UNIT_PT = 3;
    public static final int COMPLEX_UNIT_IN = 4;
    public static final int COMPLEX_UNIT_MM = 5;
    public static final int COMPLEX_UNIT_FRACTION = 0;
    public static final int COMPLEX_UNIT_FRACTION_PARENT = 1;
    public static final int COMPLEX_RADIX_SHIFT = 4;
    public static final int COMPLEX_RADIX_MASK = 0x3;
    public static final int COMPLEX_RADIX_23p0 = 0;
    public static final int COMPLEX_RADIX_16p7 = 1;
    public static final int COMPLEX_RADIX_8p15 = 2;
    public static final int COMPLEX_RADIX_0p23 = 3;
    public static final int COMPLEX_MANTISSA_SHIFT = 8;
    public static final int COMPLEX_MANTISSA_MASK = 0xffffff;
    public static final int DATA_NULL_UNDEFINED = 0;
    public static final int DATA_NULL_EMPTY = 1;
    public static final int DENSITY_DEFAULT = 0;
    public static final int DENSITY_NONE = 0xffff;

    public int type;
    public CharSequence string;
    public int data;
    public int assetCookie;
    public int resourceId;
    public int sourceResourceId;
    public int changingConfigurations = -1;
    public int density;

    private static final float MANTISSA_MULT = 1.0f / (1 << COMPLEX_MANTISSA_SHIFT);
    private static final float[] RADIX_MULTS = {
        1.0f * MANTISSA_MULT, 1.0f / (1 << 7) * MANTISSA_MULT,
        1.0f / (1 << 15) * MANTISSA_MULT, 1.0f / (1 << 23) * MANTISSA_MULT};

    public final float getFloat() {
        return Float.intBitsToFloat(data);
    }

    public boolean isColorType() {
        return type >= TYPE_FIRST_COLOR_INT && type <= TYPE_LAST_COLOR_INT;
    }

    /** The float a complex value (a dimension's or a fraction's data) holds, without its unit. */
    public static float complexToFloat(int complex) {
        return (complex & (COMPLEX_MANTISSA_MASK << COMPLEX_MANTISSA_SHIFT))
            * RADIX_MULTS[(complex >> COMPLEX_RADIX_SHIFT) & COMPLEX_RADIX_MASK];
    }

    public static float complexToDimension(int data, DisplayMetrics metrics) {
        return applyDimension((data >> COMPLEX_UNIT_SHIFT) & COMPLEX_UNIT_MASK, complexToFloat(data), metrics);
    }

    public static int complexToDimensionPixelOffset(int data, DisplayMetrics metrics) {
        return (int) complexToDimension(data, metrics);
    }

    /** A size in pixels, rounded as Android rounds: at least one pixel for a value that is not zero. */
    public static int complexToDimensionPixelSize(int data, DisplayMetrics metrics) {
        float value = complexToDimension(data, metrics);
        int res = (int) (value >= 0 ? value + 0.5f : value - 0.5f);
        if (res != 0) return res;
        if (value == 0) return 0;
        return value > 0 ? 1 : -1;
    }

    public static float complexToFraction(int data, float base, float pbase) {
        switch ((data >> COMPLEX_UNIT_SHIFT) & COMPLEX_UNIT_MASK) {
            case COMPLEX_UNIT_FRACTION: return complexToFloat(data) * base;
            case COMPLEX_UNIT_FRACTION_PARENT: return complexToFloat(data) * pbase;
            default: return 0;
        }
    }

    public static float applyDimension(int unit, float value, DisplayMetrics metrics) {
        switch (unit) {
            case COMPLEX_UNIT_PX: return value;
            case COMPLEX_UNIT_DIP: return value * metrics.density;
            case COMPLEX_UNIT_SP: return value * metrics.scaledDensity;
            case COMPLEX_UNIT_PT: return value * metrics.xdpi * (1.0f / 72);
            case COMPLEX_UNIT_IN: return value * metrics.xdpi;
            case COMPLEX_UNIT_MM: return value * metrics.xdpi * (1.0f / 25.4f);
            default: return 0;
        }
    }

    public static float deriveDimension(int unit, float value, DisplayMetrics metrics) {
        switch (unit) {
            case COMPLEX_UNIT_PX: return value;
            case COMPLEX_UNIT_DIP: return metrics.density == 0 ? 0 : value / metrics.density;
            case COMPLEX_UNIT_SP: return metrics.scaledDensity == 0 ? 0 : value / metrics.scaledDensity;
            case COMPLEX_UNIT_PT: return metrics.xdpi == 0 ? 0 : value / metrics.xdpi * 72;
            case COMPLEX_UNIT_IN: return metrics.xdpi == 0 ? 0 : value / metrics.xdpi;
            case COMPLEX_UNIT_MM: return metrics.xdpi == 0 ? 0 : value / metrics.xdpi * 25.4f;
            default: throw new IllegalArgumentException("Invalid unitToConvertTo " + unit);
        }
    }

    public static float convertPixelsToDimension(int unit, float pixels, DisplayMetrics metrics) {
        return deriveDimension(unit, pixels, metrics);
    }

    public static float convertDimensionToPixels(int unit, float value, DisplayMetrics metrics) {
        return applyDimension(unit, value, metrics);
    }

    public float getDimension(DisplayMetrics metrics) {
        return complexToDimension(data, metrics);
    }

    public int getComplexUnit() {
        return (data >> COMPLEX_UNIT_SHIFT) & COMPLEX_UNIT_MASK;
    }

    public float getFraction(float base, float pbase) {
        return complexToFraction(data, base, pbase);
    }

    public final CharSequence coerceToString() {
        return type == TYPE_STRING ? string : coerceToString(type, data);
    }

    private static final String[] DIMENSION_UNITS = {"px", "dip", "sp", "pt", "in", "mm"};
    private static final String[] FRACTION_UNITS = {"%", "%p"};

    public static final String coerceToString(int type, int data) {
        switch (type) {
            case TYPE_NULL: return null;
            case TYPE_REFERENCE: return "@" + data;
            case TYPE_ATTRIBUTE: return "?" + data;
            case TYPE_FLOAT: return Float.toString(Float.intBitsToFloat(data));
            case TYPE_DIMENSION: {
                int unit = (data >> COMPLEX_UNIT_SHIFT) & COMPLEX_UNIT_MASK;
                return complexToFloat(data) + (unit < DIMENSION_UNITS.length ? DIMENSION_UNITS[unit] : "");
            }
            case TYPE_FRACTION: {
                int unit = (data >> COMPLEX_UNIT_SHIFT) & COMPLEX_UNIT_MASK;
                return complexToFloat(data) * 100 + (unit < FRACTION_UNITS.length ? FRACTION_UNITS[unit] : "");
            }
            case TYPE_INT_HEX: return "0x" + Integer.toHexString(data);
            case TYPE_INT_BOOLEAN: return data != 0 ? "true" : "false";
            default:
                if (type >= TYPE_FIRST_COLOR_INT && type <= TYPE_LAST_COLOR_INT)
                    return "#" + String.format("%08x", data);
                if (type >= TYPE_FIRST_INT && type <= TYPE_LAST_INT) return Integer.toString(data);
                return null;
        }
    }

    public void setTo(TypedValue other) {
        type = other.type;
        string = other.string;
        data = other.data;
        assetCookie = other.assetCookie;
        resourceId = other.resourceId;
        sourceResourceId = other.sourceResourceId;
        changingConfigurations = other.changingConfigurations;
        density = other.density;
    }

    @Override
    public String toString() {
        return "TypedValue{t=0x" + Integer.toHexString(type) + "/d=0x" + Integer.toHexString(data)
            + (type == TYPE_STRING ? " \"" + string + "\"" : "") + (assetCookie != 0 ? " a=" + assetCookie : "")
            + (resourceId != 0 ? " r=0x" + Integer.toHexString(resourceId) : "") + "}";
    }
}
