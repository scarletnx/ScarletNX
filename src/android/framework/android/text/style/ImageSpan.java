// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.drawable.Drawable;

/** A drawable in place of its characters, with the source that HTML gave it. */
public class ImageSpan extends DynamicDrawableSpan {
    private final Drawable drawable;
    private final String source;

    public ImageSpan(Drawable drawable) { this(drawable, ALIGN_BOTTOM); }
    public ImageSpan(Drawable drawable, int verticalAlignment) { this(drawable, null, verticalAlignment); }
    public ImageSpan(Drawable drawable, String source) { this(drawable, source, ALIGN_BOTTOM); }
    public ImageSpan(Drawable drawable, String source, int verticalAlignment) {
        super(verticalAlignment);
        this.drawable = drawable;
        this.source = source;
    }
    public ImageSpan(android.content.Context context, android.graphics.Bitmap bitmap) { this(context, bitmap, ALIGN_BOTTOM); }
    public ImageSpan(android.content.Context context, android.graphics.Bitmap bitmap, int verticalAlignment) {
        this(bitmapDrawable(context, bitmap), verticalAlignment);
    }
    private static Drawable bitmapDrawable(android.content.Context context, android.graphics.Bitmap bitmap) {
        android.graphics.drawable.BitmapDrawable d = new android.graphics.drawable.BitmapDrawable(context != null ? context.getResources() : null, bitmap);
        d.setBounds(0, 0, Math.max(0, d.getIntrinsicWidth()), Math.max(0, d.getIntrinsicHeight()));
        return d;
    }
    @Override public Drawable getDrawable() { return drawable; }
    public String getSource() { return source; }
}
