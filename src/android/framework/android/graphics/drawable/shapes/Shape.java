// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;

/** A geometric shape, sized by resize and drawn at the origin. */
public abstract class Shape implements Cloneable {
    private float width, height;

    public Shape() {}
    public final float getWidth() { return width; }
    public final float getHeight() { return height; }
    public abstract void draw(Canvas canvas, Paint paint);
    public final void resize(float width, float height) {
        if (width < 0) width = 0;
        if (height < 0) height = 0;
        if (this.width != width || this.height != height) {
            this.width = width;
            this.height = height;
            onResize(width, height);
        }
    }
    public boolean hasAlpha() { return true; }
    protected void onResize(float width, float height) {}
    public void getOutline(Outline outline) {}
    @Override public Shape clone() throws CloneNotSupportedException { return (Shape) super.clone(); }
}
