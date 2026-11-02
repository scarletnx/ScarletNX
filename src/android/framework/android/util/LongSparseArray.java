// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.util.Arrays;

/** Objects by long key, in key order. */
public class LongSparseArray<E> implements Cloneable {
    private long[] keys = new long[10];
    private Object[] values = new Object[10];
    private int size;

    public LongSparseArray() {}
    public LongSparseArray(int capacity) { keys = new long[Math.max(capacity, 1)]; values = new Object[keys.length]; }

    public E get(long key) { return get(key, null); }
    @SuppressWarnings("unchecked")
    public E get(long key, E def) { int i = Arrays.binarySearch(keys, 0, size, key); return i >= 0 ? (E) values[i] : def; }
    public void delete(long key) { int i = Arrays.binarySearch(keys, 0, size, key); if (i >= 0) removeAt(i); }
    public void remove(long key) { delete(key); }
    public void removeAt(int index) {
        System.arraycopy(keys, index + 1, keys, index, size - index - 1);
        System.arraycopy(values, index + 1, values, index, size - index - 1);
        values[--size] = null;
    }
    public void put(long key, E value) {
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
    public long keyAt(int index) { return keys[index]; }
    @SuppressWarnings("unchecked")
    public E valueAt(int index) { return (E) values[index]; }
    public void setValueAt(int index, E value) { values[index] = value; }
    public int indexOfKey(long key) { int i = Arrays.binarySearch(keys, 0, size, key); return i >= 0 ? i : -1; }
    public void clear() { Arrays.fill(values, 0, size, null); size = 0; }
    public void append(long key, E value) { put(key, value); }
}
