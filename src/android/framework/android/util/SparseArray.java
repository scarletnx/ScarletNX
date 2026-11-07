// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.util.Arrays;

/** Objects by int key, in key order. */
public class SparseArray<E> implements Cloneable {
    int[] keys;
    Object[] values;
    int size;

    public SparseArray() { this(10); }
    public SparseArray(int capacity) { keys = new int[Math.max(capacity, 1)]; values = new Object[keys.length]; }

    @Override
    @SuppressWarnings("unchecked")
    public SparseArray<E> clone() {
        try {
            SparseArray<E> c = (SparseArray<E>) super.clone();
            c.keys = keys.clone();
            c.values = values.clone();
            return c;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }

    public boolean contains(int key) { return indexOfKey(key) >= 0; }
    public E get(int key) { return get(key, null); }
    @SuppressWarnings("unchecked")
    public E get(int key, E def) { int i = Arrays.binarySearch(keys, 0, size, key); return i >= 0 ? (E) values[i] : def; }
    public void delete(int key) { int i = Arrays.binarySearch(keys, 0, size, key); if (i >= 0) removeAt(i); }
    public void remove(int key) { delete(key); }
    public void removeAt(int index) {
        System.arraycopy(keys, index + 1, keys, index, size - index - 1);
        System.arraycopy(values, index + 1, values, index, size - index - 1);
        values[--size] = null;
    }
    public void removeAtRange(int index, int count) { for (int i = 0; i < count && index < size; ++i) removeAt(index); }
    public E removeReturnOld(int key) { E old = get(key); delete(key); return old; }
    public void set(int key, E value) { put(key, value); }

    public void put(int key, E value) {
        int i = Arrays.binarySearch(keys, 0, size, key);
        if (i >= 0) { values[i] = value; return; }
        i = ~i;
        if (size == keys.length) {
            keys = Arrays.copyOf(keys, size * 2);
            values = Arrays.copyOf(values, size * 2);
        }
        System.arraycopy(keys, i, keys, i + 1, size - i);
        System.arraycopy(values, i, values, i + 1, size - i);
        keys[i] = key;
        values[i] = value;
        ++size;
    }

    public void putAll(SparseArray<? extends E> other) { for (int i = 0; i < other.size(); ++i) put(other.keyAt(i), other.valueAt(i)); }
    public int size() { return size; }
    public int keyAt(int index) { return keys[index]; }
    @SuppressWarnings("unchecked")
    public E valueAt(int index) { return (E) values[index]; }
    public void setValueAt(int index, E value) { values[index] = value; }
    public int indexOfKey(int key) { return Arrays.binarySearch(keys, 0, size, key) >= 0 ? Arrays.binarySearch(keys, 0, size, key) : -1; }
    public int indexOfValue(E value) { for (int i = 0; i < size; ++i) if (values[i] == value) return i; return -1; }
    public void clear() { Arrays.fill(values, 0, size, null); size = 0; }
    public void append(int key, E value) { put(key, value); }
    public boolean contentEquals(SparseArray<?> other) {
        if (other == null || other.size != size) return false;
        for (int i = 0; i < size; ++i) if (keys[i] != other.keys[i] || !java.util.Objects.equals(values[i], other.values[i])) return false;
        return true;
    }
    @Override public String toString() {
        StringBuilder b = new StringBuilder("{");
        for (int i = 0; i < size; ++i) b.append(i > 0 ? ", " : "").append(keys[i]).append('=').append(values[i]);
        return b.append('}').toString();
    }
}
