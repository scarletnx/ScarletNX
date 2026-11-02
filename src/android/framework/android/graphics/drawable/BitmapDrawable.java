// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;

/** A bitmap drawn into the bounds. */
public class BitmapDrawable extends Drawable {
    private final Bitmap bitmap;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);

    public BitmapDrawable(android.content.res.Resources res, Bitmap bitmap) { this.bitmap = bitmap; }
    public BitmapDrawable(Bitmap bitmap) { this.bitmap = bitmap; }
    public final Bitmap getBitmap() { return bitmap; }
    public final Paint getPaint() { return paint; }
    @Override public void draw(Canvas canvas) { if (bitmap != null) canvas.drawBitmap(bitmap, null, getBounds(), paint); }
    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
    @Override public int getAlpha() { return paint.getAlpha(); }
    @Override public void setColorFilter(ColorFilter colorFilter) { paint.setColorFilter(colorFilter); invalidateSelf(); }
    @Override public int getOpacity() { return bitmap != null && !bitmap.hasAlpha() && paint.getAlpha() >= 255 ? PixelFormat.OPAQUE : PixelFormat.TRANSLUCENT; }
    @Override public int getIntrinsicWidth() { return bitmap != null ? bitmap.getWidth() : -1; }
    @Override public int getIntrinsicHeight() { return bitmap != null ? bitmap.getHeight() : -1; }
}
