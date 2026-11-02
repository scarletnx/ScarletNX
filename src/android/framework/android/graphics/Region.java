// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** An area: kept as its bounds, which is what the framework's users of it here need. */
public class Region {
    public enum Op { DIFFERENCE, INTERSECT, UNION, XOR, REVERSE_DIFFERENCE, REPLACE }

    private final Rect bounds = new Rect();

    public Region() {}
    public Region(Region region) { bounds.set(region.bounds); }
    public Region(Rect r) { bounds.set(r); }
    public Region(int left, int top, int right, int bottom) { bounds.set(left, top, right, bottom); }

    public void setEmpty() { bounds.setEmpty(); }
    public boolean set(Region region) { bounds.set(region.bounds); return !isEmpty(); }
    public boolean set(Rect r) { bounds.set(r); return !isEmpty(); }
    public boolean set(int left, int top, int right, int bottom) { bounds.set(left, top, right, bottom); return !isEmpty(); }
    public boolean setPath(Path path, Region clip) {
        RectF b = new RectF();
        path.computeBounds(b, true);
        Rect r = new Rect();
        b.roundOut(r);
        if (!r.intersect(clip.bounds)) r.setEmpty();
        bounds.set(r);
        return !isEmpty();
    }
    public boolean isEmpty() { return bounds.isEmpty(); }
    public boolean isRect() { return true; }
    public boolean isComplex() { return false; }
    public Rect getBounds() { return new Rect(bounds); }
    public boolean getBounds(Rect r) { r.set(bounds); return !isEmpty(); }
    public boolean contains(int x, int y) { return bounds.contains(x, y); }
    public boolean quickContains(Rect r) { return bounds.contains(r); }
    public boolean quickContains(int left, int top, int right, int bottom) { return bounds.contains(left, top, right, bottom); }
    public boolean quickReject(Rect r) { return !Rect.intersects(bounds, r); }
    public boolean quickReject(int left, int top, int right, int bottom) { return !bounds.intersects(left, top, right, bottom); }
    public void translate(int dx, int dy) { bounds.offset(dx, dy); }
    public final boolean union(Rect r) { return op(r, Op.UNION); }
    public boolean op(Rect r, Op op) { return op(bounds, r, op); }
    public boolean op(int left, int top, int right, int bottom, Op op) { return op(new Rect(left, top, right, bottom), op); }
    public boolean op(Region region, Op op) { return op(region.bounds, op); }
    public boolean op(Rect a, Rect b, Op op) {
        Rect r = new Rect(a);
        switch (op) {
            case INTERSECT: if (!r.intersect(b)) r.setEmpty(); break;
            case UNION: case XOR: r.union(b); break;
            case REPLACE: r.set(b); break;
            case REVERSE_DIFFERENCE: r.set(b); break;
            default: break;
        }
        bounds.set(r);
        return !isEmpty();
    }
    public boolean op(Rect rect, Region region, Op op) { return op(rect, region.bounds, op); }
    public boolean op(Region a, Region b, Op op) { return op(a.bounds, b.bounds, op); }
    @Override public boolean equals(Object o) { return o instanceof Region r && r.bounds.equals(bounds); }
    @Override public int hashCode() { return bounds.hashCode(); }
    @Override public String toString() { return "Region" + bounds.toShortString(); }
}
