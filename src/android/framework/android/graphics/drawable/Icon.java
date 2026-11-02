// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.PorterDuff;
import android.net.Uri;

/** An icon by its source: a bitmap, a resource, a content URI or data. Drawn through a Drawable when loaded. */
public final class Icon implements android.os.Parcelable {
    public static final int TYPE_BITMAP = 1, TYPE_RESOURCE = 2, TYPE_DATA = 3, TYPE_URI = 4, TYPE_ADAPTIVE_BITMAP = 5,
        TYPE_URI_ADAPTIVE_BITMAP = 6;

    private final int type;
    private Bitmap bitmap;
    private String resPackage, uri;
    private int resId;
    private byte[] data;
    private int dataOffset, dataLength;
    private ColorStateList tint;
    private PorterDuff.Mode tintMode = PorterDuff.Mode.SRC_IN;

    private Icon(int type) { this.type = type; }

    public static Icon createWithBitmap(Bitmap bits) { Icon i = new Icon(TYPE_BITMAP); i.bitmap = bits; return i; }
    public static Icon createWithAdaptiveBitmap(Bitmap bits) { Icon i = new Icon(TYPE_ADAPTIVE_BITMAP); i.bitmap = bits; return i; }
    public static Icon createWithResource(Context context, int resId) { return createWithResource(context.getPackageName(), resId); }
    public static Icon createWithResource(String resPackage, int resId) {
        Icon i = new Icon(TYPE_RESOURCE);
        i.resPackage = resPackage;
        i.resId = resId;
        return i;
    }
    public static Icon createWithData(byte[] data, int offset, int length) {
        Icon i = new Icon(TYPE_DATA);
        i.data = data;
        i.dataOffset = offset;
        i.dataLength = length;
        return i;
    }
    public static Icon createWithContentUri(String uri) { Icon i = new Icon(TYPE_URI); i.uri = uri; return i; }
    public static Icon createWithContentUri(Uri uri) { return createWithContentUri(uri.toString()); }
    public static Icon createWithAdaptiveBitmapContentUri(Uri uri) { Icon i = new Icon(TYPE_URI_ADAPTIVE_BITMAP); i.uri = uri.toString(); return i; }

    public int getType() { return type; }
    public int getResId() {
        if (type != TYPE_RESOURCE) throw new IllegalStateException("called getResId() on " + this);
        return resId;
    }
    public String getResPackage() {
        if (type != TYPE_RESOURCE) throw new IllegalStateException("called getResPackage() on " + this);
        return resPackage;
    }
    public Uri getUri() {
        if (type != TYPE_URI && type != TYPE_URI_ADAPTIVE_BITMAP) throw new IllegalStateException("called getUri() on " + this);
        return Uri.parse(uri);
    }
    public Icon setTint(int tint) { return setTintList(ColorStateList.valueOf(tint)); }
    public Icon setTintList(ColorStateList tintList) { tint = tintList; return this; }
    public Icon setTintMode(PorterDuff.Mode mode) { tintMode = mode; return this; }

    /** The icon's drawable: its bitmap, or its resource's drawable */
    public Drawable loadDrawable(Context context) {
        Drawable d = switch (type) {
            case TYPE_BITMAP, TYPE_ADAPTIVE_BITMAP -> new BitmapDrawable(context.getResources(), bitmap);
            case TYPE_RESOURCE -> context.getResources().getDrawable(resId, context.getTheme());
            default -> null;
        };
        if (d != null && tint != null) d.setTintList(tint);
        if (d != null && tint != null) d.setTintMode(tintMode);
        return d;
    }
    @Override public String toString() { return "Icon(typ=" + type + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
