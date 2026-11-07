// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.util;

import java.util.Objects;

/** An immutable range of comparable values, with both ends inclusive. */
public final class Range<T extends Comparable<? super T>> {
    private final T lower, upper;

    public Range(T lower, T upper) {
        this.lower = Objects.requireNonNull(lower, "lower must not be null");
        this.upper = Objects.requireNonNull(upper, "upper must not be null");
        if (lower.compareTo(upper) > 0) throw new IllegalArgumentException("lower must be less than or equal to upper");
    }

    public static <T extends Comparable<? super T>> Range<T> create(T lower, T upper) { return new Range<>(lower, upper); }

    public T getLower() { return lower; }
    public T getUpper() { return upper; }

    public boolean contains(T value) {
        Objects.requireNonNull(value, "value must not be null");
        return value.compareTo(lower) >= 0 && value.compareTo(upper) <= 0;
    }

    public boolean contains(Range<T> range) {
        Objects.requireNonNull(range, "value must not be null");
        return range.lower.compareTo(lower) >= 0 && range.upper.compareTo(upper) <= 0;
    }

    public T clamp(T value) {
        Objects.requireNonNull(value, "value must not be null");
        if (value.compareTo(lower) < 0) return lower;
        if (value.compareTo(upper) > 0) return upper;
        return value;
    }

    /** The common part of the two ranges. They must overlap. */
    public Range<T> intersect(Range<T> range) {
        Objects.requireNonNull(range, "range must not be null");
        int cmpLower = range.lower.compareTo(lower), cmpUpper = range.upper.compareTo(upper);
        if (cmpLower <= 0 && cmpUpper >= 0) return this;
        if (cmpLower >= 0 && cmpUpper <= 0) return range;
        return create(cmpLower <= 0 ? lower : range.lower, cmpUpper >= 0 ? upper : range.upper);
    }

    public Range<T> intersect(T lower, T upper) { return intersect(create(lower, upper)); }

    /** The smallest range that holds both ranges. */
    public Range<T> extend(Range<T> range) {
        Objects.requireNonNull(range, "range must not be null");
        int cmpLower = range.lower.compareTo(lower), cmpUpper = range.upper.compareTo(upper);
        if (cmpLower <= 0 && cmpUpper >= 0) return range;
        if (cmpLower >= 0 && cmpUpper <= 0) return this;
        return create(cmpLower >= 0 ? lower : range.lower, cmpUpper <= 0 ? upper : range.upper);
    }

    public Range<T> extend(T lower, T upper) { return extend(create(lower, upper)); }

    public Range<T> extend(T value) {
        Objects.requireNonNull(value, "value must not be null");
        return extend(create(value, value));
    }

    @Override public boolean equals(Object o) { return o instanceof Range<?> r && lower.equals(r.lower) && upper.equals(r.upper); }
    @Override public int hashCode() { return Objects.hash(lower, upper); }
    @Override public String toString() { return "[" + lower + ", " + upper + "]"; }
}
