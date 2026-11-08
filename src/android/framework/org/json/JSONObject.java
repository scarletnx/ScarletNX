// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package org.json;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

/** A JSON object: names in the order they were put, as Android's keeps them. */
public class JSONObject {
    /** JSON's null, apart from a missing value */
    public static final Object NULL = new Object() {
        @Override public boolean equals(Object o) { return o == this || o == null; }
        @Override public int hashCode() { return java.util.Objects.hashCode(null); }
        @Override public String toString() { return "null"; }
    };

    private final LinkedHashMap<String, Object> values;

    public JSONObject() { values = new LinkedHashMap<>(); }
    public JSONObject(Map copyFrom) {
        this();
        for (Object o : copyFrom.entrySet()) {
            Map.Entry<?, ?> e = (Map.Entry<?, ?>) o;
            String key = (String) e.getKey();
            if (key == null) throw new NullPointerException("key == null");
            values.put(key, wrap(e.getValue()));
        }
    }
    public JSONObject(JSONTokener readFrom) throws JSONException {
        Object object = readFrom.nextValue();
        if (!(object instanceof JSONObject o)) throw JSON.typeMismatch(object, "JSONObject");
        values = o.values;
    }
    public JSONObject(String json) throws JSONException { this(new JSONTokener(json)); }
    public JSONObject(JSONObject copyFrom, String[] names) throws JSONException {
        this();
        for (String name : names) {
            Object value = copyFrom.opt(name);
            if (value != null) values.put(name, value);
        }
    }

    public int length() { return values.size(); }

    public JSONObject put(String name, boolean value) throws JSONException { values.put(checkName(name), value); return this; }
    public JSONObject put(String name, double value) throws JSONException { values.put(checkName(name), JSON.checkDouble(value)); return this; }
    public JSONObject put(String name, int value) throws JSONException { values.put(checkName(name), value); return this; }
    public JSONObject put(String name, long value) throws JSONException { values.put(checkName(name), value); return this; }
    public JSONObject put(String name, Object value) throws JSONException {
        if (value == null) {
            values.remove(name);
            return this;
        }
        if (value instanceof Number n) JSON.checkDouble(n.doubleValue());
        values.put(checkName(name), value);
        return this;
    }
    public JSONObject putOpt(String name, Object value) throws JSONException {
        if (name == null || value == null) return this;
        return put(name, value);
    }
    public JSONObject accumulate(String name, Object value) throws JSONException {
        Object current = values.get(checkName(name));
        if (current == null) return put(name, value);
        if (current instanceof JSONArray a) {
            a.checkedPut(value);
        } else {
            JSONArray array = new JSONArray();
            array.checkedPut(current);
            array.checkedPut(value);
            values.put(name, array);
        }
        return this;
    }
    public JSONObject append(String name, Object value) throws JSONException {
        Object current = values.get(checkName(name));
        JSONArray array;
        if (current instanceof JSONArray a) {
            array = a;
        } else if (current == null) {
            array = new JSONArray();
            values.put(name, array);
        } else {
            throw new JSONException("Key " + name + " is not a JSONArray");
        }
        array.checkedPut(value);
        return this;
    }

    String checkName(String name) throws JSONException {
        if (name == null) throw new JSONException("Names must be non-null");
        return name;
    }

    public Object remove(String name) { return values.remove(name); }
    public boolean isNull(String name) { Object value = values.get(name); return value == null || value == NULL; }
    public boolean has(String name) { return values.containsKey(name); }

    public Object get(String name) throws JSONException {
        Object result = values.get(name);
        if (result == null) throw new JSONException("No value for " + name);
        return result;
    }
    public Object opt(String name) { return name == null ? null : values.get(name); }

    public boolean getBoolean(String name) throws JSONException {
        Object object = get(name);
        Boolean result = JSON.toBoolean(object);
        if (result == null) throw JSON.typeMismatch(name, object, "boolean");
        return result;
    }
    public boolean optBoolean(String name) { return optBoolean(name, false); }
    public boolean optBoolean(String name, boolean fallback) { Boolean r = JSON.toBoolean(opt(name)); return r != null ? r : fallback; }

    public double getDouble(String name) throws JSONException {
        Object object = get(name);
        Double result = JSON.toDouble(object);
        if (result == null) throw JSON.typeMismatch(name, object, "double");
        return result;
    }
    public double optDouble(String name) { return optDouble(name, Double.NaN); }
    public double optDouble(String name, double fallback) { Double r = JSON.toDouble(opt(name)); return r != null ? r : fallback; }

    public int getInt(String name) throws JSONException {
        Object object = get(name);
        Integer result = JSON.toInteger(object);
        if (result == null) throw JSON.typeMismatch(name, object, "int");
        return result;
    }
    public int optInt(String name) { return optInt(name, 0); }
    public int optInt(String name, int fallback) { Integer r = JSON.toInteger(opt(name)); return r != null ? r : fallback; }

    public long getLong(String name) throws JSONException {
        Object object = get(name);
        Long result = JSON.toLong(object);
        if (result == null) throw JSON.typeMismatch(name, object, "long");
        return result;
    }
    public long optLong(String name) { return optLong(name, 0L); }
    public long optLong(String name, long fallback) { Long r = JSON.toLong(opt(name)); return r != null ? r : fallback; }

    public String getString(String name) throws JSONException {
        Object object = get(name);
        String result = JSON.toString(object);
        if (result == null) throw JSON.typeMismatch(name, object, "String");
        return result;
    }
    public String optString(String name) { return optString(name, ""); }
    public String optString(String name, String fallback) { String r = JSON.toString(opt(name)); return r != null ? r : fallback; }

    public JSONArray getJSONArray(String name) throws JSONException {
        Object object = get(name);
        if (object instanceof JSONArray a) return a;
        throw JSON.typeMismatch(name, object, "JSONArray");
    }
    public JSONArray optJSONArray(String name) { return opt(name) instanceof JSONArray a ? a : null; }
    public JSONObject getJSONObject(String name) throws JSONException {
        Object object = get(name);
        if (object instanceof JSONObject o) return o;
        throw JSON.typeMismatch(name, object, "JSONObject");
    }
    public JSONObject optJSONObject(String name) { return opt(name) instanceof JSONObject o ? o : null; }

    public JSONArray toJSONArray(JSONArray names) throws JSONException {
        JSONArray result = new JSONArray();
        if (names == null) return null;
        int length = names.length();
        if (length == 0) return null;
        for (int i = 0; i < length; ++i) result.put(opt(JSON.toString(names.opt(i))));
        return result;
    }

    public Iterator<String> keys() { return values.keySet().iterator(); }
    public Set<String> keySet() { return values.keySet(); }
    public JSONArray names() { return values.isEmpty() ? null : new JSONArray(new ArrayList<>(values.keySet())); }

    @Override public String toString() {
        try {
            JSONStringer stringer = new JSONStringer();
            writeTo(stringer);
            return stringer.toString();
        } catch (JSONException e) {
            return null;
        }
    }
    public String toString(int indentSpaces) throws JSONException {
        JSONStringer stringer = new JSONStringer(indentSpaces);
        writeTo(stringer);
        return stringer.toString();
    }
    void writeTo(JSONStringer stringer) throws JSONException {
        stringer.object();
        for (Map.Entry<String, Object> e : values.entrySet()) stringer.key(e.getKey()).value(e.getValue());
        stringer.endObject();
    }

    public static String numberToString(Number number) throws JSONException {
        if (number == null) throw new JSONException("Number must be non-null");
        double d = number.doubleValue();
        JSON.checkDouble(d);
        if (number.equals(-0.0d)) return "-0";
        long longValue = number.longValue();
        if (d == (double) longValue) return Long.toString(longValue);
        return number.toString();
    }
    public static String quote(String data) {
        if (data == null) return "\"\"";
        try {
            JSONStringer stringer = new JSONStringer();
            stringer.open(JSONStringer.Scope.NULL, "");
            stringer.value(data);
            stringer.close(JSONStringer.Scope.NULL, JSONStringer.Scope.NULL, "");
            return stringer.toString();
        } catch (JSONException e) {
            throw new AssertionError();
        }
    }

    /** A Java value as a JSON one: collections and maps become arrays and objects */
    public static Object wrap(Object o) {
        if (o == null) return NULL;
        if (o instanceof JSONArray || o instanceof JSONObject || o.equals(NULL)) return o;
        try {
            if (o instanceof Collection<?> c) return new JSONArray(c);
            if (o.getClass().isArray()) return new JSONArray(o);
            if (o instanceof Map<?, ?> m) return new JSONObject(m);
            if (o instanceof Boolean || o instanceof Byte || o instanceof Character || o instanceof Double
                || o instanceof Float || o instanceof Integer || o instanceof Long || o instanceof Short
                || o instanceof String)
                return o;
            if (o.getClass().getPackage() != null && o.getClass().getPackage().getName().startsWith("java.")) return o.toString();
        } catch (Exception ignored) {}
        return null;
    }
}
