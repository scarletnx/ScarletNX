// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** Converts a value of one type to another, as an animator sets a property of a type its values are not. */
public abstract class TypeConverter<T, V> {
    private final Class<T> fromClass;
    private final Class<V> toClass;

    public TypeConverter(Class<T> fromClass, Class<V> toClass) { this.fromClass = fromClass; this.toClass = toClass; }
    public abstract V convert(T value);
    Class<V> getTargetType() { return toClass; }
    Class<T> getSourceType() { return fromClass; }
}
