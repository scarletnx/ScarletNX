// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Rect;

/** What a view's parent does for it. */
public interface ViewParent {
    void requestLayout();
    boolean isLayoutRequested();
    void requestTransparentRegion(View child);
    default void onDescendantInvalidated(View child, View target) {}
    @Deprecated void invalidateChild(View child, Rect r);
    @Deprecated ViewParent invalidateChildInParent(int[] location, Rect r);
    ViewParent getParent();
    void requestChildFocus(View child, View focused);
    void recomputeViewAttributes(View child);
    void clearChildFocus(View child);
    boolean getChildVisibleRect(View child, Rect r, android.graphics.Point offset);
    View focusSearch(View v, int direction);
    default View keyboardNavigationClusterSearch(View currentCluster, int direction) { return null; }
    void bringChildToFront(View child);
    void focusableViewAvailable(View v);
    boolean showContextMenuForChild(View originalView);
    boolean showContextMenuForChild(View originalView, float x, float y);
    default void createContextMenu(ContextMenu menu) {}
    ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback);
    ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback, int type);
    void childDrawableStateChanged(View child);
    void requestDisallowInterceptTouchEvent(boolean disallowIntercept);
    boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate);
    boolean requestSendAccessibilityEvent(View child, android.view.accessibility.AccessibilityEvent event);
    void childHasTransientStateChanged(View child, boolean hasTransientState);
    default void requestFitSystemWindows() {}
    default ViewParent getParentForAccessibility() { return getParent(); }
    void notifySubtreeAccessibilityStateChanged(View child, View source, int changeType);
    boolean canResolveLayoutDirection();
    boolean isLayoutDirectionResolved();
    int getLayoutDirection();
    default boolean canResolveTextDirection() { return true; }
    default boolean isTextDirectionResolved() { return true; }
    default int getTextDirection() { return View.TEXT_DIRECTION_FIRST_STRONG; }
    default boolean canResolveTextAlignment() { return true; }
    default boolean isTextAlignmentResolved() { return true; }
    default int getTextAlignment() { return View.TEXT_ALIGNMENT_GRAVITY; }
    boolean onStartNestedScroll(View child, View target, int nestedScrollAxes);
    void onNestedScrollAccepted(View child, View target, int nestedScrollAxes);
    void onStopNestedScroll(View target);
    void onNestedScroll(View target, int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed);
    void onNestedPreScroll(View target, int dx, int dy, int[] consumed);
    boolean onNestedFling(View target, float velocityX, float velocityY, boolean consumed);
    boolean onNestedPreFling(View target, float velocityX, float velocityY);
    boolean onNestedPrePerformAccessibilityAction(View target, int action, android.os.Bundle arguments);
}
