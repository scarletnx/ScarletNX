// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * A cache that holds at most maxSize of values, as sizeOf measures them. A get moves its value to the head of the
 * queue, and when the cache is full the value at the tail is evicted.
 */
public class LruCache<K, V> {
    private final LinkedHashMap<K, V> map = new LinkedHashMap<>(0, 0.75f, true);
    private int size, maxSize;
    private int putCount, createCount, evictionCount, hitCount, missCount;

    public LruCache(int maxSize) {
        if (maxSize <= 0) throw new IllegalArgumentException("maxSize <= 0");
        this.maxSize = maxSize;
    }

    public void resize(int maxSize) {
        if (maxSize <= 0) throw new IllegalArgumentException("maxSize <= 0");
        synchronized (this) { this.maxSize = maxSize; }
        trimToSize(maxSize);
    }

    public final V get(K key) {
        if (key == null) throw new NullPointerException("key == null");
        V mapValue;
        synchronized (this) {
            mapValue = map.get(key);
            if (mapValue != null) { ++hitCount; return mapValue; }
            ++missCount;
        }
        V created = create(key);
        if (created == null) return null;
        synchronized (this) {
            ++createCount;
            mapValue = map.put(key, created);
            if (mapValue != null) map.put(key, mapValue); else size += safeSizeOf(key, created);
        }
        if (mapValue != null) {
            entryRemoved(false, key, created, mapValue);
            return mapValue;
        }
        trimToSize(maxSize);
        return created;
    }

    public final V put(K key, V value) {
        if (key == null || value == null) throw new NullPointerException("key == null || value == null");
        V previous;
        synchronized (this) {
            ++putCount;
            size += safeSizeOf(key, value);
            previous = map.put(key, value);
            if (previous != null) size -= safeSizeOf(key, previous);
        }
        if (previous != null) entryRemoved(false, key, previous, value);
        trimToSize(maxSize);
        return previous;
    }

    public void trimToSize(int maxSize) {
        while (true) {
            K key;
            V value;
            synchronized (this) {
                if (size < 0 || (map.isEmpty() && size != 0)) throw new IllegalStateException(getClass().getName() + ".sizeOf() is reporting inconsistent results!");
                if (size <= maxSize || map.isEmpty()) break;
                Map.Entry<K, V> eldest = map.entrySet().iterator().next();
                key = eldest.getKey();
                value = eldest.getValue();
                map.remove(key);
                size -= safeSizeOf(key, value);
                ++evictionCount;
            }
            entryRemoved(true, key, value, null);
        }
    }

    public final V remove(K key) {
        if (key == null) throw new NullPointerException("key == null");
        V previous;
        synchronized (this) {
            previous = map.remove(key);
            if (previous != null) size -= safeSizeOf(key, previous);
        }
        if (previous != null) entryRemoved(false, key, previous, null);
        return previous;
    }

    protected void entryRemoved(boolean evicted, K key, V oldValue, V newValue) {}
    protected V create(K key) { return null; }

    private int safeSizeOf(K key, V value) {
        int result = sizeOf(key, value);
        if (result < 0) throw new IllegalStateException("Negative size: " + key + "=" + value);
        return result;
    }

    protected int sizeOf(K key, V value) { return 1; }

    public final void evictAll() { trimToSize(-1); }
    public final synchronized int size() { return size; }
    public final synchronized int maxSize() { return maxSize; }
    public final synchronized int hitCount() { return hitCount; }
    public final synchronized int missCount() { return missCount; }
    public final synchronized int createCount() { return createCount; }
    public final synchronized int putCount() { return putCount; }
    public final synchronized int evictionCount() { return evictionCount; }
    public final synchronized Map<K, V> snapshot() { return new LinkedHashMap<>(map); }

    @Override public final synchronized String toString() {
        int accesses = hitCount + missCount;
        int hitPercent = accesses != 0 ? 100 * hitCount / accesses : 0;
        return String.format("LruCache[maxSize=%d,hits=%d,misses=%d,hitRate=%d%%]", maxSize, hitCount, missCount, hitPercent);
    }
}
