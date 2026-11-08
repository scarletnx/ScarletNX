// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

/** An accessibility event: made, never sent. */
public final class AccessibilityEvent extends AccessibilityRecord implements android.os.Parcelable {
    public static final int TYPE_VIEW_CLICKED = 0x1, TYPE_VIEW_LONG_CLICKED = 0x2, TYPE_VIEW_SELECTED = 0x4,
        TYPE_VIEW_FOCUSED = 0x8, TYPE_VIEW_TEXT_CHANGED = 0x10, TYPE_WINDOW_STATE_CHANGED = 0x20,
        TYPE_NOTIFICATION_STATE_CHANGED = 0x40, TYPE_VIEW_HOVER_ENTER = 0x80, TYPE_VIEW_HOVER_EXIT = 0x100,
        TYPE_TOUCH_EXPLORATION_GESTURE_START = 0x200, TYPE_TOUCH_EXPLORATION_GESTURE_END = 0x400,
        TYPE_WINDOW_CONTENT_CHANGED = 0x800, TYPE_VIEW_SCROLLED = 0x1000, TYPE_VIEW_TEXT_SELECTION_CHANGED = 0x2000,
        TYPE_ANNOUNCEMENT = 0x4000, TYPE_VIEW_ACCESSIBILITY_FOCUSED = 0x8000, TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED = 0x10000,
        TYPE_VIEW_TEXT_TRAVERSED_AT_MOVEMENT_GRANULARITY = 0x20000, TYPE_GESTURE_DETECTION_START = 0x40000,
        TYPE_GESTURE_DETECTION_END = 0x80000, TYPE_TOUCH_INTERACTION_START = 0x100000, TYPE_TOUCH_INTERACTION_END = 0x200000,
        TYPE_WINDOWS_CHANGED = 0x400000, TYPE_VIEW_CONTEXT_CLICKED = 0x800000, TYPE_ASSIST_READING_CONTEXT = 0x1000000,
        TYPE_SPEECH_STATE_CHANGE = 0x2000000, TYPE_VIEW_TARGETED_BY_SCROLL = 0x4000000, TYPES_ALL_MASK = 0xffffffff;
    public static final int CONTENT_CHANGE_TYPE_UNDEFINED = 0, CONTENT_CHANGE_TYPE_SUBTREE = 1, CONTENT_CHANGE_TYPE_TEXT = 2,
        CONTENT_CHANGE_TYPE_CONTENT_DESCRIPTION = 4, CONTENT_CHANGE_TYPE_STATE_DESCRIPTION = 64,
        CONTENT_CHANGE_TYPE_PANE_TITLE = 8, CONTENT_CHANGE_TYPE_PANE_APPEARED = 16, CONTENT_CHANGE_TYPE_PANE_DISAPPEARED = 32,
        CONTENT_CHANGE_TYPE_ERROR = 256, CONTENT_CHANGE_TYPE_ENABLED = 4096;
    private int eventType, contentChangeTypes, action, movementGranularity;
    private long eventTime;
    private CharSequence packageName;

    public AccessibilityEvent() {}
    public AccessibilityEvent(int eventType) { this.eventType = eventType; }
    public AccessibilityEvent(AccessibilityEvent event) { eventType = event.eventType; }
    public static AccessibilityEvent obtain(int eventType) { return new AccessibilityEvent(eventType); }
    public static AccessibilityEvent obtain(AccessibilityEvent event) { return new AccessibilityEvent(event); }
    public static AccessibilityEvent obtain() { return new AccessibilityEvent(); }
    public int getEventType() { return eventType; }
    public void setEventType(int eventType) { this.eventType = eventType; }
    public int getContentChangeTypes() { return contentChangeTypes; }
    public void setContentChangeTypes(int changeTypes) { contentChangeTypes = changeTypes; }
    public long getEventTime() { return eventTime; }
    public void setEventTime(long eventTime) { this.eventTime = eventTime; }
    public CharSequence getPackageName() { return packageName; }
    public void setPackageName(CharSequence packageName) { this.packageName = packageName; }
    public int getAction() { return action; }
    public void setAction(int action) { this.action = action; }
    public int getMovementGranularity() { return movementGranularity; }
    public void setMovementGranularity(int granularity) { movementGranularity = granularity; }
    public int getRecordCount() { return 0; }
    public void appendRecord(AccessibilityRecord record) {}
    public AccessibilityRecord getRecord(int index) { return null; }
    public static String eventTypeToString(int eventType) { return "TYPE_" + eventType; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel parcel, int flags) {}
}
