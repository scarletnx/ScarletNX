// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2011 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

/** A mouse pointer's shape. */
public final class PointerIcon implements android.os.Parcelable {
    public static final int TYPE_NULL = 0, TYPE_ARROW = 1000, TYPE_HAND = 1002, TYPE_TEXT = 1008, TYPE_CROSSHAIR = 1007,
        TYPE_DEFAULT = TYPE_ARROW;
    private final int type;

    private PointerIcon(int type) { this.type = type; }
    public static PointerIcon getSystemIcon(android.content.Context context, int type) { return new PointerIcon(type); }
    public static PointerIcon create(android.graphics.Bitmap bitmap, float hotSpotX, float hotSpotY) { return new PointerIcon(TYPE_ARROW); }
    /** An icon from a pointer-icon resource. Its bitmap is not read: the host shows its arrow. */
    public static PointerIcon load(android.content.res.Resources resources, int resourceId) {
        if (resources == null) throw new IllegalArgumentException("resources must not be null");
        resources.getXml(resourceId).close();
        return new PointerIcon(TYPE_ARROW);
    }
    public int getType() { return type; }
    @Override public boolean equals(Object o) { return o instanceof PointerIcon p && p.type == type; }
    @Override public int hashCode() { return type; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeInt(type); }
}
