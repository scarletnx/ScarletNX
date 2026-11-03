// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.shapes.Shape;

/** A Shape drawn with a paint into the bounds. Without a shape, the bounds are filled. */
public class ShapeDrawable extends Drawable {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Shape shape;
    private int alpha = 0xff, width = -1, height = -1;
    private Rect padding;

    public ShapeDrawable() { this(null); }
    public ShapeDrawable(Shape s) {
        shape = s;
        paint.setColor(0xff000000);
    }
    public Shape getShape() { return shape; }
    public void setShape(Shape s) {
        shape = s;
        if (s != null) s.resize(getBounds().width(), getBounds().height());
        invalidateSelf();
    }
    public Paint getPaint() { return paint; }
    public void setPadding(int left, int top, int right, int bottom) { padding = (left | top | right | bottom) == 0 ? null : new Rect(left, top, right, bottom); }
    public void setPadding(Rect padding) { this.padding = padding == null ? null : new Rect(padding); }
    @Override public boolean getPadding(Rect out) {
        if (padding == null) return super.getPadding(out);
        out.set(padding);
        return true;
    }
    public void setIntrinsicWidth(int width) { this.width = width; invalidateSelf(); }
    public void setIntrinsicHeight(int height) { this.height = height; invalidateSelf(); }
    @Override public int getIntrinsicWidth() { return width; }
    @Override public int getIntrinsicHeight() { return height; }

    protected void onDraw(Shape shape, Canvas canvas, Paint paint) { shape.draw(canvas, paint); }
    @Override public void draw(Canvas canvas) {
        Rect r = getBounds();
        int old = paint.getAlpha();
        paint.setAlpha(old * (alpha + (alpha >> 7)) >> 8);
        if (paint.getAlpha() != 0 || paint.getXfermode() != null) {
            if (shape == null) {
                canvas.drawRect(r, paint);
            } else {
                int save = canvas.save();
                canvas.translate(r.left, r.top);
                onDraw(shape, canvas, paint);
                canvas.restoreToCount(save);
            }
        }
        paint.setAlpha(old);
    }
    @Override protected void onBoundsChange(Rect bounds) { if (shape != null) shape.resize(bounds.width(), bounds.height()); }
    @Override public void setAlpha(int alpha) { this.alpha = alpha; invalidateSelf(); }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { paint.setColorFilter(colorFilter); invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return paint.getColorFilter(); }
    @Override public int getOpacity() {
        if (shape == null) {
            int a = paint.getAlpha() * alpha / 255;
            return a == 0 ? PixelFormat.TRANSPARENT : a == 255 ? PixelFormat.OPAQUE : PixelFormat.TRANSLUCENT;
        }
        return PixelFormat.TRANSLUCENT;
    }
    @Override public void getOutline(Outline outline) {
        if (shape == null) { super.getOutline(outline); return; }
        shape.getOutline(outline);
        outline.offset(getBounds().left, getBounds().top);
        outline.setAlpha(alpha / 255f);
    }
}
