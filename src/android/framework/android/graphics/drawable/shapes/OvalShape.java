// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.RectF;

/** An oval in the shape's size. */
public class OvalShape extends RectShape {
    public OvalShape() {}
    @Override public void draw(Canvas canvas, Paint paint) { canvas.drawOval(rect(), paint); }
    @Override public void getOutline(Outline outline) {
        RectF r = rect();
        outline.setOval((int) Math.ceil(r.left), (int) Math.ceil(r.top), (int) Math.floor(r.right), (int) Math.floor(r.bottom));
    }
    @Override protected void onResize(float width, float height) { super.onResize(width, height); }
    @Override public OvalShape clone() throws CloneNotSupportedException { return (OvalShape) super.clone(); }
}
