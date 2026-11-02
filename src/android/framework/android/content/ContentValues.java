// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/** Column values for a content provider or a database. */
public final class ContentValues implements android.os.Parcelable {
    private final HashMap<String, Object> values;

    public ContentValues() { values = new HashMap<>(8); }
    public ContentValues(int size) { values = new HashMap<>(Math.max(size, 1)); }
    public ContentValues(ContentValues from) { values = new HashMap<>(from.values); }

    public void put(String key, String value) { values.put(key, value); }
    public void putAll(ContentValues other) { values.putAll(other.values); }
    public void put(String key, Byte value) { values.put(key, value); }
    public void put(String key, Short value) { values.put(key, value); }
    public void put(String key, Integer value) { values.put(key, value); }
    public void put(String key, Long value) { values.put(key, value); }
    public void put(String key, Float value) { values.put(key, value); }
    public void put(String key, Double value) { values.put(key, value); }
    public void put(String key, Boolean value) { values.put(key, value); }
    public void put(String key, byte[] value) { values.put(key, value); }
    public void putNull(String key) { values.put(key, null); }
    public int size() { return values.size(); }
    public boolean isEmpty() { return values.isEmpty(); }
    public void remove(String key) { values.remove(key); }
    public void clear() { values.clear(); }
    public boolean containsKey(String key) { return values.containsKey(key); }
    public Object get(String key) { return values.get(key); }
    public String getAsString(String key) { Object v = values.get(key); return v != null ? v.toString() : null; }
    public Long getAsLong(String key) { Object v = values.get(key); try { return v instanceof Number n ? n.longValue() : v != null ? Long.valueOf(v.toString()) : null; } catch (NumberFormatException e) { return null; } }
    public Integer getAsInteger(String key) { Object v = values.get(key); try { return v instanceof Number n ? n.intValue() : v != null ? Integer.valueOf(v.toString()) : null; } catch (NumberFormatException e) { return null; } }
    public Short getAsShort(String key) { Integer i = getAsInteger(key); return i != null ? i.shortValue() : null; }
    public Byte getAsByte(String key) { Integer i = getAsInteger(key); return i != null ? i.byteValue() : null; }
    public Double getAsDouble(String key) { Object v = values.get(key); try { return v instanceof Number n ? n.doubleValue() : v != null ? Double.valueOf(v.toString()) : null; } catch (NumberFormatException e) { return null; } }
    public Float getAsFloat(String key) { Double d = getAsDouble(key); return d != null ? d.floatValue() : null; }
    public Boolean getAsBoolean(String key) {
        Object v = values.get(key);
        if (v instanceof Boolean b) return b;
        if (v instanceof Number n) return n.intValue() != 0;
        return v instanceof CharSequence s ? Boolean.valueOf(s.toString()) || "1".equals(s.toString()) : null;
    }
    public byte[] getAsByteArray(String key) { Object v = values.get(key); return v instanceof byte[] b ? b : null; }
    public Set<Map.Entry<String, Object>> valueSet() { return values.entrySet(); }
    public Set<String> keySet() { return values.keySet(); }
    @Override public boolean equals(Object o) { return o instanceof ContentValues c && c.values.equals(values); }
    @Override public int hashCode() { return values.hashCode(); }
    @Override public String toString() { return values.toString(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel parcel, int flags) { parcel.writeValue(new ContentValues(this)); }
}
