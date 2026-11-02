// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A float point. */
public class PointF implements android.os.Parcelable {
    public float x, y;

    public PointF() {}
    public PointF(float x, float y) { this.x = x; this.y = y; }
    public PointF(Point p) { x = p.x; y = p.y; }
    public PointF(PointF p) { x = p.x; y = p.y; }
    public final void set(float x, float y) { this.x = x; this.y = y; }
    public final void set(PointF p) { x = p.x; y = p.y; }
    public final void negate() { x = -x; y = -y; }
    public final void offset(float dx, float dy) { x += dx; y += dy; }
    public final boolean equals(float x, float y) { return this.x == x && this.y == y; }
    public final float length() { return (float) Math.hypot(x, y); }
    public static float length(float x, float y) { return (float) Math.hypot(x, y); }
    @Override public boolean equals(Object o) { return o instanceof PointF p && p.x == x && p.y == y; }
    @Override public int hashCode() { return Float.floatToIntBits(x) * 31 + Float.floatToIntBits(y); }
    @Override public String toString() { return "PointF(" + x + ", " + y + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeFloat(x); out.writeFloat(y); }
}
