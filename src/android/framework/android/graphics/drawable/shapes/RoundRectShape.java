// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;

/** A rectangle with round corners, with an optional inner round rectangle cut out of it. */
public class RoundRectShape extends RectShape {
    private final float[] outerRadii, innerRadii;
    private final RectF inset;
    private final RectF innerRect;
    private Path path = new Path();

    public RoundRectShape(float[] outerRadii, RectF inset, float[] innerRadii) {
        if (outerRadii != null && outerRadii.length < 8) throw new ArrayIndexOutOfBoundsException("outer radii must have >= 8 values");
        if (innerRadii != null && innerRadii.length < 8) throw new ArrayIndexOutOfBoundsException("inner radii must have >= 8 values");
        this.outerRadii = outerRadii;
        this.inset = inset;
        this.innerRadii = innerRadii;
        innerRect = inset != null ? new RectF() : null;
    }
    @Override public void draw(Canvas canvas, Paint paint) { canvas.drawPath(path, paint); }
    @Override public void getOutline(Outline outline) {
        if (innerRect != null) return; /* a ring has no convex outline */
        RectF r = rect();
        float radius = outerRadii != null ? outerRadii[0] : 0;
        for (int i = 1; outerRadii != null && i < 8; ++i) {
            if (outerRadii[i] != radius) { outline.setConvexPath(path); return; }
        }
        outline.setRoundRect((int) Math.ceil(r.left), (int) Math.ceil(r.top), (int) Math.floor(r.right), (int) Math.floor(r.bottom), radius);
    }
    @Override protected void onResize(float w, float h) {
        super.onResize(w, h);
        RectF r = rect();
        path.reset();
        if (outerRadii != null) path.addRoundRect(r, outerRadii, Path.Direction.CW);
        else path.addRect(r, Path.Direction.CW);
        if (innerRect != null) {
            innerRect.set(r.left + inset.left, r.top + inset.top, r.right - inset.right, r.bottom - inset.bottom);
            if (innerRect.width() < w && innerRect.height() < h) {
                if (innerRadii != null) path.addRoundRect(innerRect, innerRadii, Path.Direction.CCW);
                else path.addRect(innerRect, Path.Direction.CCW);
            }
        }
    }
    @Override public RoundRectShape clone() throws CloneNotSupportedException {
        RoundRectShape s = (RoundRectShape) super.clone();
        s.path = new Path(path);
        return s;
    }
}
