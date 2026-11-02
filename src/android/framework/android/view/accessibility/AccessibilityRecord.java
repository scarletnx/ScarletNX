// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import android.view.View;
import java.util.ArrayList;
import java.util.List;

/** An accessibility event's record. */
public class AccessibilityRecord {
    private CharSequence className, contentDescription, beforeText;
    private final ArrayList<CharSequence> text = new ArrayList<>();
    private int itemCount, currentItemIndex, fromIndex = -1, toIndex = -1, scrollX, scrollY, maxScrollX, maxScrollY,
        addedCount = -1, removedCount = -1, scrollDeltaX, scrollDeltaY;
    private boolean checked, enabled = true, password, fullScreen, scrollable;
    private android.os.Parcelable parcelableData;

    public void setSource(View source) {}
    public void setSource(View root, int virtualDescendantId) {}
    public AccessibilityNodeInfo getSource() { return null; }
    public int getWindowId() { return -1; }
    public boolean isChecked() { return checked; }
    public void setChecked(boolean isChecked) { checked = isChecked; }
    public boolean isEnabled() { return enabled; }
    public void setEnabled(boolean isEnabled) { enabled = isEnabled; }
    public boolean isPassword() { return password; }
    public void setPassword(boolean isPassword) { password = isPassword; }
    public boolean isFullScreen() { return fullScreen; }
    public void setFullScreen(boolean isFullScreen) { fullScreen = isFullScreen; }
    public boolean isScrollable() { return scrollable; }
    public void setScrollable(boolean scrollable) { this.scrollable = scrollable; }
    public int getItemCount() { return itemCount; }
    public void setItemCount(int itemCount) { this.itemCount = itemCount; }
    public int getCurrentItemIndex() { return currentItemIndex; }
    public void setCurrentItemIndex(int currentItemIndex) { this.currentItemIndex = currentItemIndex; }
    public int getFromIndex() { return fromIndex; }
    public void setFromIndex(int fromIndex) { this.fromIndex = fromIndex; }
    public int getToIndex() { return toIndex; }
    public void setToIndex(int toIndex) { this.toIndex = toIndex; }
    public int getScrollX() { return scrollX; }
    public void setScrollX(int scrollX) { this.scrollX = scrollX; }
    public int getScrollY() { return scrollY; }
    public void setScrollY(int scrollY) { this.scrollY = scrollY; }
    public int getScrollDeltaX() { return scrollDeltaX; }
    public void setScrollDeltaX(int d) { scrollDeltaX = d; }
    public int getScrollDeltaY() { return scrollDeltaY; }
    public void setScrollDeltaY(int d) { scrollDeltaY = d; }
    public int getMaxScrollX() { return maxScrollX; }
    public void setMaxScrollX(int maxScrollX) { this.maxScrollX = maxScrollX; }
    public int getMaxScrollY() { return maxScrollY; }
    public void setMaxScrollY(int maxScrollY) { this.maxScrollY = maxScrollY; }
    public int getAddedCount() { return addedCount; }
    public void setAddedCount(int addedCount) { this.addedCount = addedCount; }
    public int getRemovedCount() { return removedCount; }
    public void setRemovedCount(int removedCount) { this.removedCount = removedCount; }
    public CharSequence getClassName() { return className; }
    public void setClassName(CharSequence className) { this.className = className; }
    public List<CharSequence> getText() { return text; }
    public CharSequence getBeforeText() { return beforeText; }
    public void setBeforeText(CharSequence beforeText) { this.beforeText = beforeText; }
    public CharSequence getContentDescription() { return contentDescription; }
    public void setContentDescription(CharSequence contentDescription) { this.contentDescription = contentDescription; }
    public android.os.Parcelable getParcelableData() { return parcelableData; }
    public void setParcelableData(android.os.Parcelable parcelableData) { this.parcelableData = parcelableData; }
    public static AccessibilityRecord obtain() { return new AccessibilityRecord(); }
    public static AccessibilityRecord obtain(AccessibilityRecord record) { return new AccessibilityRecord(); }
    public void recycle() {}
}
