// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** Four integer insets. */
public final class Insets implements android.os.Parcelable {
    public static final Insets NONE = new Insets(0, 0, 0, 0);
    public final int left, top, right, bottom;

    private Insets(int left, int top, int right, int bottom) { this.left = left; this.top = top; this.right = right; this.bottom = bottom; }
    public static Insets of(int left, int top, int right, int bottom) {
        return left == 0 && top == 0 && right == 0 && bottom == 0 ? NONE : new Insets(left, top, right, bottom);
    }
    public static Insets of(Rect r) { return r == null ? NONE : of(r.left, r.top, r.right, r.bottom); }
    public Rect toRect() { return new Rect(left, top, right, bottom); }
    public static Insets add(Insets a, Insets b) { return of(a.left + b.left, a.top + b.top, a.right + b.right, a.bottom + b.bottom); }
    public static Insets subtract(Insets a, Insets b) { return of(a.left - b.left, a.top - b.top, a.right - b.right, a.bottom - b.bottom); }
    public static Insets max(Insets a, Insets b) { return of(Math.max(a.left, b.left), Math.max(a.top, b.top), Math.max(a.right, b.right), Math.max(a.bottom, b.bottom)); }
    public static Insets min(Insets a, Insets b) { return of(Math.min(a.left, b.left), Math.min(a.top, b.top), Math.min(a.right, b.right), Math.min(a.bottom, b.bottom)); }
    @Override public boolean equals(Object o) { return o instanceof Insets i && i.left == left && i.top == top && i.right == right && i.bottom == bottom; }
    @Override public int hashCode() { return ((left * 31 + top) * 31 + right) * 31 + bottom; }
    @Override public String toString() { return "Insets{left=" + left + ", top=" + top + ", right=" + right + ", bottom=" + bottom + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeInt(left); out.writeInt(top); out.writeInt(right); out.writeInt(bottom); }
}
