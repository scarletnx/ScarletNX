// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** A named property of a class, read and written through methods. */
public abstract class Property<T, V> {
    private final String name;
    private final Class<V> type;

    public Property(Class<V> type, String name) { this.name = name; this.type = type; }
    public boolean isReadOnly() { return false; }
    public void set(T object, V value) { throw new UnsupportedOperationException("Property " + name + " is read-only"); }
    public abstract V get(T object);
    public String getName() { return name; }
    public Class<V> getType() { return type; }
    public static <T, V> Property<T, V> of(Class<T> hostType, Class<V> valueType, String name) {
        throw new UnsupportedOperationException("snx: Property.of");
    }
}
