// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** An integer point. */
public class Point implements android.os.Parcelable {
    public int x, y;

    public Point() {}
    public Point(int x, int y) { this.x = x; this.y = y; }
    public Point(Point p) { x = p.x; y = p.y; }
    public void set(int x, int y) { this.x = x; this.y = y; }
    public final void negate() { x = -x; y = -y; }
    public final void offset(int dx, int dy) { x += dx; y += dy; }
    public final boolean equals(int x, int y) { return this.x == x && this.y == y; }
    @Override public boolean equals(Object o) { return o instanceof Point p && p.x == x && p.y == y; }
    @Override public int hashCode() { return x * 31 + y; }
    @Override public String toString() { return "Point(" + x + ", " + y + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeInt(x); out.writeInt(y); }
}
