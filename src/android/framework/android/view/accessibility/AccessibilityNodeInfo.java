// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import android.graphics.Rect;
import android.os.Bundle;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

/** A node of the accessibility tree: made by views and their delegates, read by no service here. */
public class AccessibilityNodeInfo implements android.os.Parcelable {
    public static final int ACTION_FOCUS = 1;
    public static final int ACTION_CLEAR_FOCUS = 2;
    public static final int ACTION_SELECT = 4;
    public static final int ACTION_CLEAR_SELECTION = 8;
    public static final int ACTION_CLICK = 16;
    public static final int ACTION_LONG_CLICK = 32;
    public static final int ACTION_ACCESSIBILITY_FOCUS = 64;
    public static final int ACTION_CLEAR_ACCESSIBILITY_FOCUS = 128;
    public static final int ACTION_NEXT_AT_MOVEMENT_GRANULARITY = 256;
    public static final int ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY = 512;
    public static final int ACTION_NEXT_HTML_ELEMENT = 1024;
    public static final int ACTION_PREVIOUS_HTML_ELEMENT = 2048;
    public static final int ACTION_SCROLL_FORWARD = 4096;
    public static final int ACTION_SCROLL_BACKWARD = 8192;
    public static final int ACTION_COPY = 16384;
    public static final int ACTION_PASTE = 32768;
    public static final int ACTION_CUT = 65536;
    public static final int ACTION_SET_SELECTION = 131072;
    public static final int ACTION_EXPAND = 262144;
    public static final int ACTION_COLLAPSE = 524288;
    public static final int ACTION_DISMISS = 1048576;
    public static final int ACTION_SET_TEXT = 2097152;
    public static final int FOCUS_INPUT = 1, FOCUS_ACCESSIBILITY = 2;
    public static final int MOVEMENT_GRANULARITY_CHARACTER = 1, MOVEMENT_GRANULARITY_WORD = 2, MOVEMENT_GRANULARITY_LINE = 4,
        MOVEMENT_GRANULARITY_PARAGRAPH = 8, MOVEMENT_GRANULARITY_PAGE = 16;
    public static final String ACTION_ARGUMENT_SELECTION_START_INT = "ACTION_ARGUMENT_SELECTION_START_INT",
        ACTION_ARGUMENT_SELECTION_END_INT = "ACTION_ARGUMENT_SELECTION_END_INT",
        ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE = "ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE",
        ACTION_ARGUMENT_PROGRESS_VALUE = "android.view.accessibility.action.ARGUMENT_PROGRESS_VALUE",
        ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT = "ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT",
        ACTION_ARGUMENT_EXTEND_SELECTION_BOOLEAN = "ACTION_ARGUMENT_EXTEND_SELECTION_BOOLEAN",
        EXTRA_DATA_TEXT_CHARACTER_LOCATION_KEY = "android.view.accessibility.extra.DATA_TEXT_CHARACTER_LOCATION_KEY",
        EXTRA_DATA_TEXT_CHARACTER_LOCATION_ARG_START_INDEX = "android.view.accessibility.extra.DATA_TEXT_CHARACTER_LOCATION_ARG_START_INDEX",
        EXTRA_DATA_TEXT_CHARACTER_LOCATION_ARG_LENGTH = "android.view.accessibility.extra.DATA_TEXT_CHARACTER_LOCATION_ARG_LENGTH";
    public static final int EXTRA_DATA_TEXT_CHARACTER_LOCATION_ARG_MAX_LENGTH = 20000;

    public static final class AccessibilityAction implements android.os.Parcelable {
        public static final AccessibilityAction ACTION_FOCUS = new AccessibilityAction(1, null);
        public static final AccessibilityAction ACTION_CLEAR_FOCUS = new AccessibilityAction(2, null);
        public static final AccessibilityAction ACTION_SELECT = new AccessibilityAction(4, null);
        public static final AccessibilityAction ACTION_CLEAR_SELECTION = new AccessibilityAction(8, null);
        public static final AccessibilityAction ACTION_CLICK = new AccessibilityAction(16, null);
        public static final AccessibilityAction ACTION_LONG_CLICK = new AccessibilityAction(32, null);
        public static final AccessibilityAction ACTION_ACCESSIBILITY_FOCUS = new AccessibilityAction(64, null);
        public static final AccessibilityAction ACTION_CLEAR_ACCESSIBILITY_FOCUS = new AccessibilityAction(128, null);
        public static final AccessibilityAction ACTION_NEXT_AT_MOVEMENT_GRANULARITY = new AccessibilityAction(256, null);
        public static final AccessibilityAction ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY = new AccessibilityAction(512, null);
        public static final AccessibilityAction ACTION_NEXT_HTML_ELEMENT = new AccessibilityAction(1024, null);
        public static final AccessibilityAction ACTION_PREVIOUS_HTML_ELEMENT = new AccessibilityAction(2048, null);
        public static final AccessibilityAction ACTION_SCROLL_FORWARD = new AccessibilityAction(4096, null);
        public static final AccessibilityAction ACTION_SCROLL_BACKWARD = new AccessibilityAction(8192, null);
        public static final AccessibilityAction ACTION_COPY = new AccessibilityAction(16384, null);
        public static final AccessibilityAction ACTION_PASTE = new AccessibilityAction(32768, null);
        public static final AccessibilityAction ACTION_CUT = new AccessibilityAction(65536, null);
        public static final AccessibilityAction ACTION_SET_SELECTION = new AccessibilityAction(131072, null);
        public static final AccessibilityAction ACTION_EXPAND = new AccessibilityAction(262144, null);
        public static final AccessibilityAction ACTION_COLLAPSE = new AccessibilityAction(524288, null);
        public static final AccessibilityAction ACTION_DISMISS = new AccessibilityAction(1048576, null);
        public static final AccessibilityAction ACTION_SET_TEXT = new AccessibilityAction(2097152, null);
        public static final AccessibilityAction ACTION_SHOW_ON_SCREEN = new AccessibilityAction(16908342, null);
        public static final AccessibilityAction ACTION_SCROLL_TO_POSITION = new AccessibilityAction(16908343, null);
        public static final AccessibilityAction ACTION_SCROLL_UP = new AccessibilityAction(16908344, null);
        public static final AccessibilityAction ACTION_SCROLL_LEFT = new AccessibilityAction(16908345, null);
        public static final AccessibilityAction ACTION_SCROLL_DOWN = new AccessibilityAction(16908346, null);
        public static final AccessibilityAction ACTION_SCROLL_RIGHT = new AccessibilityAction(16908347, null);
        public static final AccessibilityAction ACTION_CONTEXT_CLICK = new AccessibilityAction(16908348, null);
        public static final AccessibilityAction ACTION_SET_PROGRESS = new AccessibilityAction(16908349, null);
        public static final AccessibilityAction ACTION_MOVE_WINDOW = new AccessibilityAction(16908354, null);
        public static final AccessibilityAction ACTION_SHOW_TOOLTIP = new AccessibilityAction(16908356, null);
        public static final AccessibilityAction ACTION_HIDE_TOOLTIP = new AccessibilityAction(16908357, null);
        public static final AccessibilityAction ACTION_PAGE_UP = new AccessibilityAction(16908358, null);
        public static final AccessibilityAction ACTION_PAGE_DOWN = new AccessibilityAction(16908359, null);
        public static final AccessibilityAction ACTION_PAGE_LEFT = new AccessibilityAction(16908360, null);
        public static final AccessibilityAction ACTION_PAGE_RIGHT = new AccessibilityAction(16908361, null);
        public static final AccessibilityAction ACTION_PRESS_AND_HOLD = new AccessibilityAction(16908362, null);
        public static final AccessibilityAction ACTION_IME_ENTER = new AccessibilityAction(16908372, null);
        public static final AccessibilityAction ACTION_DRAG_START = new AccessibilityAction(16908374, null);
        public static final AccessibilityAction ACTION_DRAG_DROP = new AccessibilityAction(16908375, null);
        public static final AccessibilityAction ACTION_DRAG_CANCEL = new AccessibilityAction(16908376, null);
        public static final AccessibilityAction ACTION_SHOW_TEXT_SUGGESTIONS = new AccessibilityAction(16908377, null);
        public static final AccessibilityAction ACTION_SCROLL_IN_DIRECTION = new AccessibilityAction(16908378, null);
        private final int id;
        private final CharSequence label;

        public AccessibilityAction(int actionId, CharSequence label) { id = actionId; this.label = label; }
        public AccessibilityAction(int actionId) { this(actionId, null); }
        public int getId() { return id; }
        public CharSequence getLabel() { return label; }
        @Override public boolean equals(Object o) { return o instanceof AccessibilityAction a && a.id == id; }
        @Override public int hashCode() { return id; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeInt(id); }
    }

    public static final class CollectionInfo {
        public static final int SELECTION_MODE_NONE = 0, SELECTION_MODE_SINGLE = 1, SELECTION_MODE_MULTIPLE = 2;
        private final int rows, columns;
        public CollectionInfo(int rowCount, int columnCount, boolean hierarchical) { rows = rowCount; columns = columnCount; }
        public CollectionInfo(int rowCount, int columnCount, boolean hierarchical, int selectionMode) { this(rowCount, columnCount, hierarchical); }
        public static CollectionInfo obtain(int rowCount, int columnCount, boolean hierarchical) { return new CollectionInfo(rowCount, columnCount, hierarchical); }
        public static CollectionInfo obtain(int rowCount, int columnCount, boolean hierarchical, int selectionMode) { return new CollectionInfo(rowCount, columnCount, hierarchical); }
        public int getRowCount() { return rows; }
        public int getColumnCount() { return columns; }
        public boolean isHierarchical() { return false; }
        public int getSelectionMode() { return SELECTION_MODE_NONE; }
    }

    public static final class CollectionItemInfo {
        private final int rowIndex, rowSpan, columnIndex, columnSpan;
        private final boolean selected;
        public CollectionItemInfo(int rowIndex, int rowSpan, int columnIndex, int columnSpan, boolean heading) { this(rowIndex, rowSpan, columnIndex, columnSpan, heading, false); }
        public CollectionItemInfo(int rowIndex, int rowSpan, int columnIndex, int columnSpan, boolean heading, boolean selected) {
            this.rowIndex = rowIndex; this.rowSpan = rowSpan; this.columnIndex = columnIndex; this.columnSpan = columnSpan; this.selected = selected;
        }
        public static CollectionItemInfo obtain(int rowIndex, int rowSpan, int columnIndex, int columnSpan, boolean heading) { return new CollectionItemInfo(rowIndex, rowSpan, columnIndex, columnSpan, heading); }
        public static CollectionItemInfo obtain(int rowIndex, int rowSpan, int columnIndex, int columnSpan, boolean heading, boolean selected) { return new CollectionItemInfo(rowIndex, rowSpan, columnIndex, columnSpan, heading, selected); }
        public int getRowIndex() { return rowIndex; }
        public String getRowTitle() { return null; }
        public String getColumnTitle() { return null; }
        public int getRowSpan() { return rowSpan; }
        public int getColumnIndex() { return columnIndex; }
        public int getColumnSpan() { return columnSpan; }
        public boolean isHeading() { return false; }
        public boolean isSelected() { return selected; }

        public static final class Builder {
            private int rowIndex, rowSpan = 1, columnIndex, columnSpan = 1;
            private boolean selected;
            public Builder() {}
            public Builder setHeading(boolean heading) { return this; }
            public Builder setColumnIndex(int columnIndex) { this.columnIndex = columnIndex; return this; }
            public Builder setRowIndex(int rowIndex) { this.rowIndex = rowIndex; return this; }
            public Builder setColumnSpan(int columnSpan) { this.columnSpan = columnSpan; return this; }
            public Builder setRowSpan(int rowSpan) { this.rowSpan = rowSpan; return this; }
            public Builder setSelected(boolean selected) { this.selected = selected; return this; }
            public Builder setRowTitle(String rowTitle) { return this; }
            public Builder setColumnTitle(String columnTitle) { return this; }
            public CollectionItemInfo build() { return new CollectionItemInfo(rowIndex, rowSpan, columnIndex, columnSpan, false, selected); }
        }
    }

    public static final class RangeInfo {
        public static final int RANGE_TYPE_INT = 0, RANGE_TYPE_FLOAT = 1, RANGE_TYPE_PERCENT = 2, RANGE_TYPE_INDETERMINATE = 3;
        private final int type;
        private final float min, max, current;
        public RangeInfo(int type, float min, float max, float current) { this.type = type; this.min = min; this.max = max; this.current = current; }
        public static RangeInfo obtain(int type, float min, float max, float current) { return new RangeInfo(type, min, max, current); }
        public int getType() { return type; }
        public float getMin() { return min; }
        public float getMax() { return max; }
        public float getCurrent() { return current; }
    }

    public static final class ExtraRenderingInfo {
        public android.util.Size getLayoutSize() { return null; }
        public float getTextSizeInPx() { return 0; }
        public int getTextSizeUnit() { return 0; }
    }

    private final ArrayList<AccessibilityAction> actions = new ArrayList<>();
    private final Bundle extras = new Bundle();
    private final Rect boundsInParent = new Rect(), boundsInScreen = new Rect();
    private CharSequence className, packageName, text, contentDescription, hintText, error, paneTitle, stateDescription,
        tooltipText, uniqueId, containerTitle;
    private String viewIdResourceName;
    private boolean visible, enabled = true, focusable, focused, selected, clickable, longClickable, checkable, checked,
        scrollable, password, editable, multiLine, dismissable, heading, importantForAccessibility = true,
        screenReaderFocusable, showingHint, textEntryKey, accessibilityFocused, contextClickable, contentInvalid;
    private int movementGranularities, textSelectionStart = -1, textSelectionEnd = -1, maxTextLength = -1, inputType,
        liveRegion, drawingOrder;
    private CollectionInfo collectionInfo;
    private CollectionItemInfo collectionItemInfo;
    private RangeInfo rangeInfo;
    private List<String> availableExtraData = new ArrayList<>();

    public AccessibilityNodeInfo() {}
    public AccessibilityNodeInfo(View source) {}
    public AccessibilityNodeInfo(View root, int virtualDescendantId) {}
    public AccessibilityNodeInfo(AccessibilityNodeInfo info) {}
    public static AccessibilityNodeInfo obtain() { return new AccessibilityNodeInfo(); }
    public static AccessibilityNodeInfo obtain(View source) { return new AccessibilityNodeInfo(source); }
    public static AccessibilityNodeInfo obtain(View root, int virtualDescendantId) { return new AccessibilityNodeInfo(root, virtualDescendantId); }
    public static AccessibilityNodeInfo obtain(AccessibilityNodeInfo info) { return new AccessibilityNodeInfo(info); }
    public void recycle() {}
    public void setSource(View source) {}
    public void setSource(View root, int virtualDescendantId) {}
    public void setParent(View parent) {}
    public void setParent(View root, int virtualDescendantId) {}
    public void addChild(View child) {}
    public void addChild(View root, int virtualDescendantId) {}
    public boolean removeChild(View child) { return false; }
    public boolean removeChild(View root, int virtualDescendantId) { return false; }
    public int getChildCount() { return 0; }
    public AccessibilityNodeInfo getChild(int index) { return null; }
    public AccessibilityNodeInfo getParent() { return null; }
    public AccessibilityNodeInfo getChild(int index, int prefetchingStrategy) { return null; }
    public AccessibilityNodeInfo getParent(int prefetchingStrategy) { return null; }
    public java.time.Duration getMinDurationBetweenContentChanges() { return java.time.Duration.ZERO; }
    public boolean hasRequestInitialAccessibilityFocus() { return false; }
    public boolean isAccessibilityDataSensitive() { return false; }
    public void setAccessibilityDataSensitive(boolean accessibilityDataSensitive) {}
    public void setLabelFor(View labeled) {}
    public void setLabelFor(View root, int virtualDescendantId) {}
    public void setLabeledBy(View label) {}
    public void setLabeledBy(View root, int virtualDescendantId) {}
    public void setTraversalBefore(View view) {}
    public void setTraversalBefore(View root, int virtualDescendantId) {}
    public void setTraversalAfter(View view) {}
    public void setTraversalAfter(View root, int virtualDescendantId) {}
    public int getWindowId() { return -1; }
    public long getSourceNodeId() { return 0; }
    public void addAction(AccessibilityAction action) { actions.add(action); }
    @Deprecated public void addAction(int action) { actions.add(new AccessibilityAction(action, null)); }
    public boolean removeAction(AccessibilityAction action) { return actions.remove(action); }
    public List<AccessibilityAction> getActionList() { return actions; }
    @Deprecated public int getActions() { int a = 0; for (AccessibilityAction x : actions) if (x.getId() < 0x200000 * 2) a |= x.getId(); return a; }
    public boolean performAction(int action) { return false; }
    public boolean performAction(int action, Bundle arguments) { return false; }
    public void setMovementGranularities(int granularities) { movementGranularities = granularities; }
    public int getMovementGranularities() { return movementGranularities; }
    public Bundle getExtras() { return extras; }
    public void setAvailableExtraData(List<String> extraDataKeys) { availableExtraData = extraDataKeys; }
    public List<String> getAvailableExtraData() { return availableExtraData; }
    @Deprecated public void getBoundsInParent(Rect outBounds) { outBounds.set(boundsInParent); }
    @Deprecated public void setBoundsInParent(Rect bounds) { boundsInParent.set(bounds); }
    public void getBoundsInScreen(Rect outBounds) { outBounds.set(boundsInScreen); }
    public void setBoundsInScreen(Rect bounds) { boundsInScreen.set(bounds); }
    public void getBoundsInWindow(Rect outBounds) { outBounds.set(boundsInScreen); }
    public void setBoundsInWindow(Rect bounds) { boundsInScreen.set(bounds); }
    public CharSequence getClassName() { return className; }
    public void setClassName(CharSequence v) { className = v; }
    public CharSequence getPackageName() { return packageName; }
    public void setPackageName(CharSequence v) { packageName = v; }
    public CharSequence getText() { return text; }
    public void setText(CharSequence v) { text = v; }
    public CharSequence getContentDescription() { return contentDescription; }
    public void setContentDescription(CharSequence v) { contentDescription = v; }
    public CharSequence getHintText() { return hintText; }
    public void setHintText(CharSequence v) { hintText = v; }
    public CharSequence getError() { return error; }
    public void setError(CharSequence v) { error = v; }
    public CharSequence getPaneTitle() { return paneTitle; }
    public void setPaneTitle(CharSequence v) { paneTitle = v; }
    public CharSequence getStateDescription() { return stateDescription; }
    public void setStateDescription(CharSequence v) { stateDescription = v; }
    public CharSequence getTooltipText() { return tooltipText; }
    public void setTooltipText(CharSequence v) { tooltipText = v; }
    public String getUniqueId() { return uniqueId != null ? uniqueId.toString() : null; }
    public void setUniqueId(String v) { uniqueId = v; }
    public CharSequence getContainerTitle() { return containerTitle; }
    public void setContainerTitle(CharSequence v) { containerTitle = v; }
    public String getViewIdResourceName() { return viewIdResourceName; }
    public void setViewIdResourceName(String v) { viewIdResourceName = v; }
    public boolean isVisibleToUser() { return visible; }
    public void setVisibleToUser(boolean v) { visible = v; }
    public boolean isEnabled() { return enabled; }
    public void setEnabled(boolean v) { enabled = v; }
    public boolean isFocusable() { return focusable; }
    public void setFocusable(boolean v) { focusable = v; }
    public boolean isFocused() { return focused; }
    public void setFocused(boolean v) { focused = v; }
    public boolean isSelected() { return selected; }
    public void setSelected(boolean v) { selected = v; }
    public boolean isClickable() { return clickable; }
    public void setClickable(boolean v) { clickable = v; }
    public boolean isLongClickable() { return longClickable; }
    public void setLongClickable(boolean v) { longClickable = v; }
    public boolean isContextClickable() { return contextClickable; }
    public void setContextClickable(boolean v) { contextClickable = v; }
    public boolean isCheckable() { return checkable; }
    public void setCheckable(boolean v) { checkable = v; }
    public boolean isChecked() { return checked; }
    public void setChecked(boolean v) { checked = v; }
    public boolean isScrollable() { return scrollable; }
    public void setScrollable(boolean v) { scrollable = v; }
    public boolean isPassword() { return password; }
    public void setPassword(boolean v) { password = v; }
    public boolean isEditable() { return editable; }
    public void setEditable(boolean v) { editable = v; }
    public boolean isMultiLine() { return multiLine; }
    public void setMultiLine(boolean v) { multiLine = v; }
    public boolean isDismissable() { return dismissable; }
    public void setDismissable(boolean v) { dismissable = v; }
    public boolean isHeading() { return heading; }
    public void setHeading(boolean v) { heading = v; }
    public boolean isImportantForAccessibility() { return importantForAccessibility; }
    public void setImportantForAccessibility(boolean v) { importantForAccessibility = v; }
    public boolean isScreenReaderFocusable() { return screenReaderFocusable; }
    public void setScreenReaderFocusable(boolean v) { screenReaderFocusable = v; }
    public boolean isShowingHintText() { return showingHint; }
    public void setShowingHintText(boolean v) { showingHint = v; }
    public boolean isTextEntryKey() { return textEntryKey; }
    public void setTextEntryKey(boolean v) { textEntryKey = v; }
    public boolean isAccessibilityFocused() { return accessibilityFocused; }
    public void setAccessibilityFocused(boolean v) { accessibilityFocused = v; }
    public boolean isContentInvalid() { return contentInvalid; }
    public void setContentInvalid(boolean v) { contentInvalid = v; }
    public boolean isTextSelectable() { return false; }
    public void setTextSelectable(boolean v) {}
    public boolean isGranularScrollingSupported() { return false; }
    public void setGranularScrollingSupported(boolean v) {}
    public void setRequestInitialAccessibilityFocus(boolean v) {}
    public void setMinDurationBetweenContentChanges(java.time.Duration d) {}
    public void setQueryFromAppProcessEnabled(View view, boolean enabled) {}
    public int getTextSelectionStart() { return textSelectionStart; }
    public int getTextSelectionEnd() { return textSelectionEnd; }
    public void setTextSelection(int start, int end) { textSelectionStart = start; textSelectionEnd = end; }
    public int getMaxTextLength() { return maxTextLength; }
    public void setMaxTextLength(int v) { maxTextLength = v; }
    public int getInputType() { return inputType; }
    public void setInputType(int v) { inputType = v; }
    public int getLiveRegion() { return liveRegion; }
    public void setLiveRegion(int v) { liveRegion = v; }
    public int getDrawingOrder() { return drawingOrder; }
    public void setDrawingOrder(int v) { drawingOrder = v; }
    public CollectionInfo getCollectionInfo() { return collectionInfo; }
    public void setCollectionInfo(CollectionInfo v) { collectionInfo = v; }
    public CollectionItemInfo getCollectionItemInfo() { return collectionItemInfo; }
    public void setCollectionItemInfo(CollectionItemInfo v) { collectionItemInfo = v; }
    public RangeInfo getRangeInfo() { return rangeInfo; }
    public void setRangeInfo(RangeInfo v) { rangeInfo = v; }
    public ExtraRenderingInfo getExtraRenderingInfo() { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel parcel, int flags) {}
}
