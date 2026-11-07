// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.Serializable;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Set;

/** The keyed values of a Bundle or a PersistableBundle, in the order they were put. */
public class BaseBundle {
    final LinkedHashMap<String, Object> map;
    ClassLoader classLoader;

    BaseBundle() { map = new LinkedHashMap<>(); }
    BaseBundle(int capacity) { map = new LinkedHashMap<>(Math.max(capacity, 1)); }
    BaseBundle(BaseBundle b) { map = new LinkedHashMap<>(b.map); classLoader = b.classLoader; }

    public int size() { return map.size(); }
    public boolean isEmpty() { return map.isEmpty(); }
    public void clear() { map.clear(); }
    public boolean containsKey(String key) { return map.containsKey(key); }
    public Object get(String key) { return map.get(key); }
    public void remove(String key) { map.remove(key); }
    public Set<String> keySet() { return map.keySet(); }
    public void putAll(PersistableBundle bundle) { map.putAll(bundle.map); }

    @SuppressWarnings("unchecked")
    <T> T typed(String key, Class<T> type) {
        Object o = map.get(key);
        return type.isInstance(o) ? (T) o : null;
    }

    public void putBoolean(String key, boolean value) { map.put(key, value); }
    public void putByte(String key, byte value) { map.put(key, value); }
    public void putChar(String key, char value) { map.put(key, value); }
    public void putShort(String key, short value) { map.put(key, value); }
    public void putInt(String key, int value) { map.put(key, value); }
    public void putLong(String key, long value) { map.put(key, value); }
    public void putFloat(String key, float value) { map.put(key, value); }
    public void putDouble(String key, double value) { map.put(key, value); }
    public void putString(String key, String value) { map.put(key, value); }
    public void putCharSequence(String key, CharSequence value) { map.put(key, value); }
    public void putBooleanArray(String key, boolean[] value) { map.put(key, value); }
    public void putIntArray(String key, int[] value) { map.put(key, value); }
    public void putLongArray(String key, long[] value) { map.put(key, value); }
    public void putDoubleArray(String key, double[] value) { map.put(key, value); }
    public void putStringArray(String key, String[] value) { map.put(key, value); }
    public void putSerializable(String key, Serializable value) { map.put(key, value); }
    public void putIntegerArrayList(String key, ArrayList<Integer> value) { map.put(key, value); }
    public void putStringArrayList(String key, ArrayList<String> value) { map.put(key, value); }

    public boolean getBoolean(String key) { return getBoolean(key, false); }
    public boolean getBoolean(String key, boolean def) { Boolean v = typed(key, Boolean.class); return v != null ? v : def; }
    public byte getByte(String key) { return getByte(key, (byte) 0); }
    public Byte getByte(String key, byte def) { Byte v = typed(key, Byte.class); return v != null ? v : def; }
    public char getChar(String key) { return getChar(key, (char) 0); }
    public char getChar(String key, char def) { Character v = typed(key, Character.class); return v != null ? v : def; }
    public short getShort(String key) { return getShort(key, (short) 0); }
    public short getShort(String key, short def) { Short v = typed(key, Short.class); return v != null ? v : def; }
    public int getInt(String key) { return getInt(key, 0); }
    public int getInt(String key, int def) { Integer v = typed(key, Integer.class); return v != null ? v : def; }
    public long getLong(String key) { return getLong(key, 0); }
    public long getLong(String key, long def) { Long v = typed(key, Long.class); return v != null ? v : def; }
    public float getFloat(String key) { return getFloat(key, 0); }
    public float getFloat(String key, float def) { Float v = typed(key, Float.class); return v != null ? v : def; }
    public double getDouble(String key) { return getDouble(key, 0); }
    public double getDouble(String key, double def) { Double v = typed(key, Double.class); return v != null ? v : def; }
    public String getString(String key) { return typed(key, String.class); }
    public String getString(String key, String def) { String v = getString(key); return v != null ? v : def; }
    public CharSequence getCharSequence(String key) { return typed(key, CharSequence.class); }
    public CharSequence getCharSequence(String key, CharSequence def) {
        CharSequence v = getCharSequence(key);
        return v != null ? v : def;
    }
    public boolean[] getBooleanArray(String key) { return typed(key, boolean[].class); }
    public int[] getIntArray(String key) { return typed(key, int[].class); }
    public long[] getLongArray(String key) { return typed(key, long[].class); }
    public double[] getDoubleArray(String key) { return typed(key, double[].class); }
    public String[] getStringArray(String key) { return typed(key, String[].class); }
    public Serializable getSerializable(String key) { return typed(key, Serializable.class); }
    public <T extends Serializable> T getSerializable(String key, Class<T> clazz) { return typed(key, clazz); }
    @SuppressWarnings("unchecked")
    public ArrayList<Integer> getIntegerArrayList(String key) { return typed(key, ArrayList.class); }
    @SuppressWarnings("unchecked")
    public ArrayList<String> getStringArrayList(String key) { return typed(key, ArrayList.class); }

    @Override
    public String toString() {
        return getClass().getSimpleName() + map;
    }
}
