// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.os.Parcel;
import android.text.ParcelableSpan;
import android.text.TextPaint;

/** A font family, style, size and colors for text. */
public class TextAppearanceSpan extends MetricAffectingSpan implements ParcelableSpan {
    private final String family;
    private final int style, size;
    private final ColorStateList color, linkColor;

    public TextAppearanceSpan(String family, int style, int size, ColorStateList color, ColorStateList linkColor) {
        this.family = family;
        this.style = style;
        this.size = size;
        this.color = color;
        this.linkColor = linkColor;
    }
    public String getFamily() { return family; }
    public ColorStateList getTextColor() { return color; }
    public ColorStateList getLinkTextColor() { return linkColor; }
    public int getTextSize() { return size; }
    public int getTextStyle() { return style; }
    @Override public int getSpanTypeId() { return 17; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(family);
        dest.writeInt(style);
        dest.writeInt(size);
    }
    @Override public void updateDrawState(TextPaint ds) {
        updateMeasureState(ds);
        if (color != null) ds.setColor(color.getColorForState(ds.drawableState, 0));
        if (linkColor != null) ds.linkColor = linkColor.getColorForState(ds.drawableState, 0);
    }
    @Override public void updateMeasureState(TextPaint ds) {
        if (family != null || style != 0) {
            Typeface tf = ds.getTypeface();
            int old = tf != null ? tf.getStyle() : 0;
            ds.setTypeface(family != null ? Typeface.create(family, style | old) : Typeface.create(tf, style | old));
        }
        if (size > 0) ds.setTextSize(size);
    }
}
