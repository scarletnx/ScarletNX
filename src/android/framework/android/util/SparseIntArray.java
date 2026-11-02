// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.util.Arrays;

/** ints by int key, in key order. */
public class SparseIntArray implements Cloneable {
    private int[] keys = new int[10];
    private int[] values = new int[10];
    private int size;

    public SparseIntArray() {}
    public SparseIntArray(int capacity) { keys = new int[Math.max(capacity, 1)]; values = new int[keys.length]; }

    @Override
    public SparseIntArray clone() {
        SparseIntArray c = new SparseIntArray();
        c.keys = keys.clone();
        c.values = values.clone();
        c.size = size;
        return c;
    }

    public int get(int key) { return get(key, (int) (Object) defaultValue()); }
    private static int defaultValue() { return intDefault.VALUE; }
    public int get(int key, int def) { int i = Arrays.binarySearch(keys, 0, size, key); return i >= 0 ? values[i] : def; }
    public void delete(int key) { int i = Arrays.binarySearch(keys, 0, size, key); if (i >= 0) removeAt(i); }
    public void removeAt(int index) {
        System.arraycopy(keys, index + 1, keys, index, size - index - 1);
        System.arraycopy(values, index + 1, values, index, size - index - 1);
        --size;
    }
    public void put(int key, int value) {
        int i = Arrays.binarySearch(keys, 0, size, key);
        if (i >= 0) { values[i] = value; return; }
        i = ~i;
        if (size == keys.length) { keys = Arrays.copyOf(keys, size * 2); values = Arrays.copyOf(values, size * 2); }
        System.arraycopy(keys, i, keys, i + 1, size - i);
        System.arraycopy(values, i, values, i + 1, size - i);
        keys[i] = key;
        values[i] = value;
        ++size;
    }
    public int size() { return size; }
    public int keyAt(int index) { return keys[index]; }
    public int valueAt(int index) { return values[index]; }
    public void setValueAt(int index, int value) { values[index] = value; }
    public int indexOfKey(int key) { int i = Arrays.binarySearch(keys, 0, size, key); return i >= 0 ? i : -1; }
    public int indexOfValue(int value) { for (int i = 0; i < size; ++i) if (values[i] == value) return i; return -1; }
    public void clear() { size = 0; }
    public void append(int key, int value) { put(key, value); }
    public int[] copyKeys() { return size == 0 ? null : Arrays.copyOf(keys, size); }

    private static final class intDefault {
        static final int VALUE = 0;
    }
}
