// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.NinePatch;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;

/** A nine-patch image stretched to its bounds, with the padding of its chunk. */
public class NinePatchDrawable extends Drawable {
    private final NinePatch patch;
    private final Rect padding;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);

    public NinePatchDrawable(Bitmap bitmap, byte[] chunk, Rect padding, String srcName) { this(null, bitmap, chunk, padding, srcName); }
    public NinePatchDrawable(Resources res, Bitmap bitmap, byte[] chunk, Rect padding, String srcName) {
        this(new NinePatch(bitmap, chunk, srcName), padding);
    }
    public NinePatchDrawable(NinePatch patch) { this(patch, null); }
    public NinePatchDrawable(Resources res, NinePatch patch) { this(patch, null); }
    private NinePatchDrawable(NinePatch patch, Rect padding) {
        this.patch = patch;
        this.padding = padding != null ? new Rect(padding) : null;
    }
    @Override public void draw(Canvas canvas) { canvas.drawPatch(patch, getBounds(), paint); }
    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
    @Override public int getAlpha() { return paint.getAlpha(); }
    @Override public void setColorFilter(ColorFilter colorFilter) { paint.setColorFilter(colorFilter); invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return paint.getColorFilter(); }
    public Paint getPaint() { return paint; }
    @Override public int getOpacity() { return patch.getBitmap().hasAlpha() || paint.getAlpha() < 255 ? PixelFormat.TRANSLUCENT : PixelFormat.OPAQUE; }
    @Override public int getIntrinsicWidth() { return patch.getWidth(); }
    @Override public int getIntrinsicHeight() { return patch.getHeight(); }
    @Override public boolean getPadding(Rect out) {
        if (padding == null) return super.getPadding(out);
        out.set(padding);
        return (padding.left | padding.top | padding.right | padding.bottom) != 0;
    }
}
