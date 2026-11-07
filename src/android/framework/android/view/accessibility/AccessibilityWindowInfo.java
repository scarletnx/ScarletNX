// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import android.graphics.Rect;
import android.graphics.Region;

/** A window as an accessibility service sees it. No service exists here, so a window has no content or place. */
public final class AccessibilityWindowInfo implements android.os.Parcelable {
    public static final int TYPE_APPLICATION = 1, TYPE_INPUT_METHOD = 2, TYPE_SYSTEM = 3, TYPE_ACCESSIBILITY_OVERLAY = 4,
        TYPE_SPLIT_SCREEN_DIVIDER = 5, TYPE_MAGNIFICATION_OVERLAY = 6;
    private int type = -1, layer = -1, id = -1, displayId = -1;
    private CharSequence title;
    private final Rect bounds = new Rect();
    private boolean active, focused, accessibilityFocused, pictureInPicture;

    public AccessibilityWindowInfo() {}
    public AccessibilityWindowInfo(AccessibilityWindowInfo info) {
        type = info.type;
        layer = info.layer;
        id = info.id;
        displayId = info.displayId;
        title = info.title;
        bounds.set(info.bounds);
        active = info.active;
        focused = info.focused;
        accessibilityFocused = info.accessibilityFocused;
        pictureInPicture = info.pictureInPicture;
    }
    public static AccessibilityWindowInfo obtain() { return new AccessibilityWindowInfo(); }
    public static AccessibilityWindowInfo obtain(AccessibilityWindowInfo info) { return new AccessibilityWindowInfo(info); }
    public void recycle() {}
    public int getType() { return type; }
    public int getLayer() { return layer; }
    public int getId() { return id; }
    public int getDisplayId() { return displayId; }
    public CharSequence getTitle() { return title; }
    public AccessibilityNodeInfo getRoot() { return null; }
    public AccessibilityNodeInfo getRoot(int prefetchingStrategy) { return null; }
    public AccessibilityNodeInfo getAnchor() { return null; }
    public AccessibilityWindowInfo getParent() { return null; }
    public int getChildCount() { return 0; }
    public AccessibilityWindowInfo getChild(int index) { throw new IndexOutOfBoundsException("index " + index + " of 0 children"); }
    public void getBoundsInScreen(Rect outBounds) { outBounds.set(bounds); }
    public void getRegionInScreen(Region outRegion) { outRegion.set(bounds); }
    public boolean isActive() { return active; }
    public boolean isFocused() { return focused; }
    public boolean isAccessibilityFocused() { return accessibilityFocused; }
    public boolean isInPictureInPictureMode() { return pictureInPicture; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
