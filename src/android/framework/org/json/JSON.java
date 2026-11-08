// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package org.json;

/** The conversions JSONObject and JSONArray share, as Android's coerce values. */
final class JSON {
    private JSON() {}

    static double checkDouble(double d) throws JSONException {
        if (Double.isInfinite(d) || Double.isNaN(d)) throw new JSONException("Forbidden numeric value: " + d);
        return d;
    }
    static Boolean toBoolean(Object value) {
        if (value instanceof Boolean b) return b;
        if (value instanceof String s) {
            if ("true".equalsIgnoreCase(s)) return true;
            if ("false".equalsIgnoreCase(s)) return false;
        }
        return null;
    }
    static Double toDouble(Object value) {
        if (value instanceof Double d) return d;
        if (value instanceof Number n) return n.doubleValue();
        if (value instanceof String s) {
            try { return Double.valueOf(s); } catch (NumberFormatException ignored) {}
        }
        return null;
    }
    static Integer toInteger(Object value) {
        if (value instanceof Integer i) return i;
        if (value instanceof Number n) return n.intValue();
        if (value instanceof String s) {
            try { return (int) Double.parseDouble(s); } catch (NumberFormatException ignored) {}
        }
        return null;
    }
    static Long toLong(Object value) {
        if (value instanceof Long l) return l;
        if (value instanceof Number n) return n.longValue();
        if (value instanceof String s) {
            try { return (long) Double.parseDouble(s); } catch (NumberFormatException ignored) {}
        }
        return null;
    }
    static String toString(Object value) {
        if (value instanceof String s) return s;
        return value != null ? String.valueOf(value) : null;
    }
    static JSONException typeMismatch(Object indexOrName, Object actual, String requiredType) throws JSONException {
        if (actual == null) throw new JSONException("Value at " + indexOrName + " is null.");
        throw new JSONException("Value " + actual + " at " + indexOrName + " of type " + actual.getClass().getName()
            + " cannot be converted to " + requiredType);
    }
    static JSONException typeMismatch(Object actual, String requiredType) throws JSONException {
        if (actual == null) throw new JSONException("Value is null.");
        throw new JSONException("Value " + actual + " of type " + actual.getClass().getName()
            + " cannot be converted to " + requiredType);
    }
}
