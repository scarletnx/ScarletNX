// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Shader;
import android.view.Gravity;

/** A bitmap drawn into the bounds. */
public class BitmapDrawable extends Drawable {
    private Bitmap bitmap;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private int gravity = Gravity.FILL;
    private Shader.TileMode tileX, tileY;
    private final Rect dst = new Rect();

    public BitmapDrawable(android.content.res.Resources res, Bitmap bitmap) { this.bitmap = bitmap; }
    public BitmapDrawable(Bitmap bitmap) { this.bitmap = bitmap; }
    BitmapDrawable() {}

    /** android:src, gravity and tileMode */
    private static final int[] BITMAP_ATTRS = {0x01010119, 0x010100af, 0x01010201};

    /** a <bitmap>: the image of its source, where its gravity puts it, or tiled */
    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, BITMAP_ATTRS);
        try {
            int src = a.getResourceId(0, 0);
            if (src == 0) throw new org.xmlpull.v1.XmlPullParserException(parser.getPositionDescription() + ": <bitmap> requires a valid 'src' attribute");
            if (r.getDrawable(src, theme) instanceof BitmapDrawable b) bitmap = b.bitmap;
            gravity = a.getInt(1, Gravity.FILL);
            int mode = a.getInt(2, -1); // disabled -1, clamp 0, repeat 1, mirror 2
            if (mode >= 0) {
                Shader.TileMode t = Shader.TileMode.values()[mode];
                setTileModeXY(t, t);
            }
        } finally {
            a.recycle();
        }
        DrawableXml.skip(parser);
    }

    public final Bitmap getBitmap() { return bitmap; }
    public final Paint getPaint() { return paint; }
    public int getGravity() { return gravity; }
    public void setGravity(int gravity) { this.gravity = gravity; invalidateSelf(); }
    public Shader.TileMode getTileModeX() { return tileX; }
    public Shader.TileMode getTileModeY() { return tileY; }
    public void setTileModeX(Shader.TileMode mode) { setTileModeXY(mode, tileY); }
    public final void setTileModeY(Shader.TileMode mode) { setTileModeXY(tileX, mode); }
    public void setTileModeXY(Shader.TileMode xmode, Shader.TileMode ymode) {
        tileX = xmode;
        tileY = ymode;
        paint.setShader(bitmap != null && (xmode != null || ymode != null)
            ? new android.graphics.BitmapShader(bitmap, xmode != null ? xmode : Shader.TileMode.CLAMP, ymode != null ? ymode : Shader.TileMode.CLAMP) : null);
        invalidateSelf();
    }
    /** A tiled bitmap fills the bounds from their corner; otherwise the bitmap goes where its gravity puts it. */
    @Override public void draw(Canvas canvas) {
        if (bitmap == null) return;
        Rect bounds = getBounds();
        if (paint.getShader() != null) {
            int save = canvas.save();
            canvas.translate(bounds.left, bounds.top);
            canvas.drawRect(0, 0, bounds.width(), bounds.height(), paint);
            canvas.restoreToCount(save);
            return;
        }
        Gravity.apply(gravity, bitmap.getWidth(), bitmap.getHeight(), bounds, dst, getLayoutDirection());
        canvas.drawBitmap(bitmap, null, dst, paint);
    }
    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
    @Override public int getAlpha() { return paint.getAlpha(); }
    @Override public void setColorFilter(ColorFilter colorFilter) { paint.setColorFilter(colorFilter); invalidateSelf(); }
    @Override public int getOpacity() { return bitmap != null && !bitmap.hasAlpha() && paint.getAlpha() >= 255 ? PixelFormat.OPAQUE : PixelFormat.TRANSLUCENT; }
    @Override public int getIntrinsicWidth() { return bitmap != null ? bitmap.getWidth() : -1; }
    @Override public int getIntrinsicHeight() { return bitmap != null ? bitmap.getHeight() : -1; }
}
