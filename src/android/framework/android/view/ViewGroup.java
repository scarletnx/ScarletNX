// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RenderNode;
import android.os.Parcelable;
import android.util.AttributeSet;
import android.util.SparseArray;
import java.util.ArrayList;

/** A view of child views: their layout params, measure, layout, drawing and the dispatch of input to them. */
public abstract class ViewGroup extends View implements ViewParent, ViewManager {
    public static final int FOCUS_BEFORE_DESCENDANTS = 0x20000, FOCUS_AFTER_DESCENDANTS = 0x40000,
        FOCUS_BLOCK_DESCENDANTS = 0x60000;
    public static final int PERSISTENT_NO_CACHE = 0, LAYOUT_MODE_CLIP_BOUNDS = 0, LAYOUT_MODE_OPTICAL_BOUNDS = 1;

    private final ArrayList<View> children = new ArrayList<>();
    private boolean clipChildren = true, clipToPadding = true, disallowIntercept, motionSplitting = true;
    private int descendantFocusability = FOCUS_BEFORE_DESCENDANTS;
    private View focusedChild;
    private View touchTarget; // the child the current gesture goes to
    private OnHierarchyChangeListener hierarchyListener;
    private int layoutMode;

    public ViewGroup(Context context) { super(context); }
    public ViewGroup(Context context, AttributeSet attrs) { super(context, attrs); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public interface OnHierarchyChangeListener {
        void onChildViewAdded(View parent, View child);

        void onChildViewRemoved(View parent, View child);
    }

    public static class LayoutParams {
        public static final int FILL_PARENT = -1, MATCH_PARENT = -1, WRAP_CONTENT = -2;
        public int width, height;
        public android.view.animation.LayoutAnimationController.AnimationParameters layoutAnimationParameters;

        public LayoutParams(Context c, AttributeSet attrs) {
            width = attrs != null ? attrs.getAttributeIntValue("http://schemas.android.com/apk/res/android", "layout_width", WRAP_CONTENT) : WRAP_CONTENT;
            height = attrs != null ? attrs.getAttributeIntValue("http://schemas.android.com/apk/res/android", "layout_height", WRAP_CONTENT) : WRAP_CONTENT;
        }
        public LayoutParams(int width, int height) { this.width = width; this.height = height; }
        public LayoutParams(LayoutParams source) { width = source.width; height = source.height; }
        protected void setBaseAttributes(TypedArray a, int widthAttr, int heightAttr) {
            width = a.getLayoutDimension(widthAttr, "layout_width");
            height = a.getLayoutDimension(heightAttr, "layout_height");
        }
        public void resolveLayoutDirection(int layoutDirection) {}
        @Override public String toString() { return "LayoutParams{" + width + ", " + height + "}"; }
    }

    public static class MarginLayoutParams extends LayoutParams {
        public int leftMargin, topMargin, rightMargin, bottomMargin;

        public MarginLayoutParams(Context c, AttributeSet attrs) { super(c, attrs); }
        public MarginLayoutParams(int width, int height) { super(width, height); }
        public MarginLayoutParams(MarginLayoutParams source) {
            super(source);
            leftMargin = source.leftMargin; topMargin = source.topMargin; rightMargin = source.rightMargin; bottomMargin = source.bottomMargin;
        }
        public MarginLayoutParams(LayoutParams source) { super(source); }
        public void setMargins(int left, int top, int right, int bottom) { leftMargin = left; topMargin = top; rightMargin = right; bottomMargin = bottom; }
        public void setMarginStart(int start) { leftMargin = start; }
        public void setMarginEnd(int end) { rightMargin = end; }
        public int getMarginStart() { return leftMargin; }
        public int getMarginEnd() { return rightMargin; }
        public boolean isMarginRelative() { return false; }
        public int getLayoutDirection() { return layoutDirection; }
        public void setLayoutDirection(int layoutDirection) {
            if (layoutDirection == LAYOUT_DIRECTION_LTR || layoutDirection == LAYOUT_DIRECTION_RTL) this.layoutDirection = layoutDirection;
        }
        private int layoutDirection = LAYOUT_DIRECTION_LTR;
    }

    // ---- children ----
    public int getChildCount() { return children.size(); }
    public View getChildAt(int index) { return index >= 0 && index < children.size() ? children.get(index) : null; }
    public int indexOfChild(View child) { return children.indexOf(child); }
    public void setOnHierarchyChangeListener(OnHierarchyChangeListener listener) { hierarchyListener = listener; }
    public void addView(View child) { addView(child, -1); }
    public void addView(View child, int index) {
        LayoutParams params = child.getLayoutParams();
        if (params == null) params = generateDefaultLayoutParams();
        addView(child, index, params);
    }
    public void addView(View child, int width, int height) {
        LayoutParams params = generateDefaultLayoutParams();
        params.width = width;
        params.height = height;
        addView(child, -1, params);
    }
    @Override public void addView(View child, LayoutParams params) { addView(child, -1, params); }
    public void addView(View child, int index, LayoutParams params) {
        requestLayout();
        invalidate();
        addViewInner(child, index, params, false);
    }
    protected boolean addViewInLayout(View child, int index, LayoutParams params) { return addViewInLayout(child, index, params, false); }
    protected boolean addViewInLayout(View child, int index, LayoutParams params, boolean preventRequestLayout) {
        addViewInner(child, index, params, preventRequestLayout);
        return true;
    }
    private void addViewInner(View child, int index, LayoutParams params, boolean preventRequestLayout) {
        if (child.mParent != null)
            throw new IllegalStateException("The specified child already has a parent. You must call removeView() on the child's parent first.");
        if (!checkLayoutParams(params)) params = generateLayoutParams(params);
        child.setLayoutParamsQuietly(params);
        if (index < 0 || index > children.size()) index = children.size();
        children.add(index, child);
        child.mParent = this;
        if (!preventRequestLayout) child.requestLayout();
        if (mAttachInfo != null) child.dispatchAttachedToWindow(mAttachInfo, getVisibility());
        onViewAdded(child);
        if (hierarchyListener != null) hierarchyListener.onChildViewAdded(this, child);
    }
    protected void onViewAdded(View child) {}
    protected void onViewRemoved(View child) {}
    protected boolean checkLayoutParams(LayoutParams p) { return p != null; }
    protected LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT); }
    protected LayoutParams generateLayoutParams(LayoutParams p) { return p; }
    public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    void onSetLayoutParams(View child, LayoutParams params) { requestLayout(); }

    @Override public void updateViewLayout(View view, LayoutParams params) {
        if (!checkLayoutParams(params)) throw new IllegalArgumentException("Invalid LayoutParams supplied to " + this);
        if (view.mParent != this) throw new IllegalArgumentException("Given view not a child of " + this);
        view.setLayoutParams(params);
    }
    @Override public void removeView(View view) {
        int i = children.indexOf(view);
        if (i >= 0) { removeViewAt(i); }
    }
    public void removeViewInLayout(View view) { int i = children.indexOf(view); if (i >= 0) removeViewInternal(i); }
    public void removeViewsInLayout(int start, int count) { for (int i = 0; i < count; ++i) removeViewInternal(start); }
    public void removeViewAt(int index) { removeViewInternal(index); requestLayout(); invalidate(); }
    public void removeViews(int start, int count) { removeViewsInLayout(start, count); requestLayout(); invalidate(); }
    public void removeAllViews() { removeAllViewsInLayout(); requestLayout(); invalidate(); }
    public void removeAllViewsInLayout() { while (!children.isEmpty()) removeViewInternal(children.size() - 1); }
    protected void removeDetachedView(View child, boolean animate) {}
    private void removeViewInternal(int index) {
        View child = children.get(index);
        if (child == focusedChild) { child.clearFocus(); focusedChild = null; }
        if (child == touchTarget) touchTarget = null;
        if (child.mAttachInfo != null) child.dispatchDetachedFromWindow();
        children.remove(index);
        child.mParent = null;
        onViewRemoved(child);
        if (hierarchyListener != null) hierarchyListener.onChildViewRemoved(this, child);
    }
    public void cleanupLayoutState(View child) { child.forceLayout(); }
    public void startViewTransition(View view) {}
    public void endViewTransition(View view) {}
    public void bringChildToFront(View child) {
        int i = children.indexOf(child);
        if (i >= 0) { children.remove(i); children.add(child); requestLayout(); invalidate(); }
    }
    @Override protected <T extends View> T findViewTraversal(int id) {
        T v = super.findViewTraversal(id);
        if (v != null) return v;
        for (View c : children) { v = c.findViewTraversal(id); if (v != null) return v; }
        return null;
    }
    @Override protected <T extends View> T findViewWithTagTraversal(Object tag) {
        T v = super.findViewWithTagTraversal(tag);
        if (v != null) return v;
        for (View c : children) { v = c.findViewWithTagTraversal(tag); if (v != null) return v; }
        return null;
    }

    // ---- attach ----
    @Override void dispatchAttachedToWindow(AttachInfo info, int visibility) {
        super.dispatchAttachedToWindow(info, visibility);
        for (View c : new ArrayList<>(children)) c.dispatchAttachedToWindow(info, visibility);
    }
    @Override void dispatchDetachedFromWindow() {
        for (View c : new ArrayList<>(children)) c.dispatchDetachedFromWindow();
        super.dispatchDetachedFromWindow();
    }
    @Override public void dispatchWindowFocusChanged(boolean hasFocus) {
        super.dispatchWindowFocusChanged(hasFocus);
        for (View c : new ArrayList<>(children)) c.dispatchWindowFocusChanged(hasFocus);
    }
    @Override public void dispatchWindowVisibilityChanged(int visibility) {
        super.dispatchWindowVisibilityChanged(visibility);
        for (View c : new ArrayList<>(children)) c.dispatchWindowVisibilityChanged(visibility);
    }
    @Override public void dispatchConfigurationChanged(android.content.res.Configuration newConfig) {
        super.dispatchConfigurationChanged(newConfig);
        for (View c : new ArrayList<>(children)) c.dispatchConfigurationChanged(newConfig);
    }
    @Override void dispatchVisibilityChanged(View changedView, int visibility) {
        super.dispatchVisibilityChanged(changedView, visibility);
        for (View c : new ArrayList<>(children)) c.dispatchVisibilityChanged(changedView, visibility);
    }
    void onChildVisibilityChanged(View child, int visibility) {}

    // ---- measure ----
    protected void measureChildren(int widthMeasureSpec, int heightMeasureSpec) {
        for (View c : children) if (c.getVisibility() != GONE) measureChild(c, widthMeasureSpec, heightMeasureSpec);
    }
    protected void measureChild(View child, int parentWidthMeasureSpec, int parentHeightMeasureSpec) {
        LayoutParams lp = child.getLayoutParams();
        child.measure(getChildMeasureSpec(parentWidthMeasureSpec, getPaddingLeft() + getPaddingRight(), lp.width),
            getChildMeasureSpec(parentHeightMeasureSpec, getPaddingTop() + getPaddingBottom(), lp.height));
    }
    protected void measureChildWithMargins(View child, int parentWidthMeasureSpec, int widthUsed, int parentHeightMeasureSpec, int heightUsed) {
        MarginLayoutParams lp = (MarginLayoutParams) child.getLayoutParams();
        child.measure(getChildMeasureSpec(parentWidthMeasureSpec, getPaddingLeft() + getPaddingRight() + lp.leftMargin + lp.rightMargin + widthUsed, lp.width),
            getChildMeasureSpec(parentHeightMeasureSpec, getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin + heightUsed, lp.height));
    }
    public static int getChildMeasureSpec(int spec, int padding, int childDimension) {
        int mode = MeasureSpec.getMode(spec), size = Math.max(0, MeasureSpec.getSize(spec) - padding);
        int resultSize = 0, resultMode = 0;
        switch (mode) {
            case MeasureSpec.EXACTLY:
                if (childDimension >= 0) { resultSize = childDimension; resultMode = MeasureSpec.EXACTLY; }
                else if (childDimension == LayoutParams.MATCH_PARENT) { resultSize = size; resultMode = MeasureSpec.EXACTLY; }
                else if (childDimension == LayoutParams.WRAP_CONTENT) { resultSize = size; resultMode = MeasureSpec.AT_MOST; }
                break;
            case MeasureSpec.AT_MOST:
                if (childDimension >= 0) { resultSize = childDimension; resultMode = MeasureSpec.EXACTLY; }
                else { resultSize = size; resultMode = MeasureSpec.AT_MOST; }
                break;
            default:
                if (childDimension >= 0) { resultSize = childDimension; resultMode = MeasureSpec.EXACTLY; }
                else { resultSize = 0; resultMode = MeasureSpec.UNSPECIFIED; }
                break;
        }
        return MeasureSpec.makeMeasureSpec(resultSize, resultMode);
    }
    @Override protected abstract void onLayout(boolean changed, int l, int t, int r, int b);
    @Override public boolean isLayoutRequested() { return super.isLayoutRequested(); }
    public boolean shouldDelayChildPressedState() { return true; }
    public void setClipChildren(boolean clipChildren) { this.clipChildren = clipChildren; renderNode().setClipToBounds(clipChildren); invalidate(); }
    public boolean getClipChildren() { return clipChildren; }
    public void setClipToPadding(boolean clipToPadding) { this.clipToPadding = clipToPadding; invalidate(); }
    public boolean getClipToPadding() { return clipToPadding; }
    public void setMotionEventSplittingEnabled(boolean split) { motionSplitting = split; }
    public boolean isMotionEventSplittingEnabled() { return motionSplitting; }
    public int getLayoutMode() { return layoutMode; }
    public void setLayoutMode(int layoutMode) { this.layoutMode = layoutMode; }
    public void setAddStatesFromChildren(boolean addsStates) {}
    public void setLayoutTransition(android.animation.LayoutTransition transition) { layoutTransition = transition; }
    public android.animation.LayoutTransition getLayoutTransition() { return layoutTransition; }
    private android.animation.LayoutTransition layoutTransition;
    protected int getChildDrawingOrder(int childCount, int drawingPosition) { return drawingPosition; }
    public void setTransitionGroup(boolean isTransitionGroup) { super.setTransitionGroup(isTransitionGroup); }
    public void setPersistentDrawingCache(int drawingCacheToKeep) {}
    public void setChildrenDrawingOrderEnabled(boolean enabled) {}

    // ---- drawing ----
    @Override protected void dispatchDraw(Canvas canvas) {
        boolean clip = clipToPadding && (getPaddingLeft() | getPaddingTop() | getPaddingRight() | getPaddingBottom()) != 0;
        if (clip) {
            canvas.save();
            canvas.clipRect(getScrollX() + getPaddingLeft(), getScrollY() + getPaddingTop(),
                getScrollX() + getWidth() - getPaddingRight(), getScrollY() + getHeight() - getPaddingBottom());
        }
        long time = getDrawingTime();
        // children with a Z above 0 last, as Android's reordering draws them
        ArrayList<View> order = new ArrayList<>(children);
        order.sort((a, b) -> Float.compare(a.getZ(), b.getZ()));
        for (View c : order) if (c.getVisibility() == VISIBLE || c.getAnimationMatrix() != null) drawChild(canvas, c, time);
        if (clip) canvas.restore();
    }
    protected boolean drawChild(Canvas canvas, View child, long drawingTime) {
        RenderNode node = child.updateDisplayListIfDirty();
        node.setClipToBounds(child instanceof ViewGroup g ? g.clipChildren && clipChildren : clipChildren);
        canvas.drawRenderNode(node);
        return false;
    }
    /** The children's lists brought up to date when this one's stays. */
    void updateChildren() { for (View c : children) if (c.getVisibility() == VISIBLE) c.updateDisplayListIfDirty(); }
    @Override public void onDescendantInvalidated(View child, View target) {
        if (target == child && child.displayListDirty() || target != child) {
            // the child records again; this list draws the same node
        }
        if (getParent() != null) getParent().onDescendantInvalidated(this, target);
    }
    @Override public void invalidateChild(View child, Rect dirty) { onDescendantInvalidated(child, child); }
    @Override public ViewParent invalidateChildInParent(int[] location, Rect dirty) { return getParent(); }
    public void childDrawableStateChanged(View child) {}
    @Override public void childHasTransientStateChanged(View child, boolean hasTransientState) {}
    public void setStaticTransformationsEnabled(boolean enabled) {}

    // ---- touch ----
    @Override public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {
        this.disallowIntercept = disallowIntercept;
        if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(disallowIntercept);
    }
    @Override public boolean onInterceptTouchEvent(MotionEvent ev) { return false; }

    /** Whether the point (this view's coordinates) is in the child, and the point in the child's coordinates. */
    private static boolean inChild(View child, float x, float y, float[] local) {
        local[0] = x - child.getLeft();
        local[1] = y - child.getTop();
        if (!child.hasIdentityMatrix()) {
            Matrix inverse = new Matrix();
            if (child.getMatrix().invert(inverse)) inverse.mapPoints(local);
        }
        return local[0] >= 0 && local[1] >= 0 && local[0] < child.getWidth() && local[1] < child.getHeight();
    }

    private boolean dispatchToChild(View child, MotionEvent ev) {
        float[] local = new float[2];
        inChild(child, ev.getX() + getScrollX(), ev.getY() + getScrollY(), local);
        MotionEvent e = MotionEvent.obtain(ev);
        e.offsetLocation(local[0] - ev.getX(), local[1] - ev.getY());
        try {
            return child.dispatchTouchEvent(e);
        } finally {
            e.recycle();
        }
    }

    @Override public boolean dispatchTouchEvent(MotionEvent ev) {
        int action = ev.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) { touchTarget = null; disallowIntercept = false; }
        boolean intercepted = false;
        if (action == MotionEvent.ACTION_DOWN || touchTarget != null) intercepted = !disallowIntercept && onInterceptTouchEvent(ev);
        else intercepted = true;
        if (intercepted && touchTarget != null) {
            MotionEvent cancel = MotionEvent.obtain(ev);
            cancel.setAction(MotionEvent.ACTION_CANCEL);
            dispatchToChild(touchTarget, cancel);
            touchTarget = null;
            return super.dispatchTouchEvent(ev);
        }
        if (!intercepted && action == MotionEvent.ACTION_DOWN) {
            float[] local = new float[2];
            ArrayList<View> order = new ArrayList<>(children);
            order.sort((a, b) -> Float.compare(a.getZ(), b.getZ()));
            for (int i = order.size() - 1; i >= 0; --i) {
                View c = order.get(i);
                if (c.getVisibility() != VISIBLE) continue;
                if (!inChild(c, ev.getX() + getScrollX(), ev.getY() + getScrollY(), local)) continue;
                if (dispatchToChild(c, ev)) { touchTarget = c; return true; }
            }
        }
        if (touchTarget != null) {
            boolean handled = dispatchToChild(touchTarget, ev);
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) touchTarget = null;
            return handled;
        }
        return super.dispatchTouchEvent(ev);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        float[] local = new float[2];
        for (int i = children.size() - 1; i >= 0; --i) {
            View c = children.get(i);
            if (c.getVisibility() == VISIBLE && inChild(c, event.getX() + getScrollX(), event.getY() + getScrollY(), local)) {
                MotionEvent e = MotionEvent.obtain(event);
                e.offsetLocation(local[0] - event.getX(), local[1] - event.getY());
                if (c.dispatchGenericMotionEvent(e)) return true;
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }
    @Override protected boolean dispatchHoverEvent(MotionEvent event) {
        float[] local = new float[2];
        for (int i = children.size() - 1; i >= 0; --i) {
            View c = children.get(i);
            if (c.getVisibility() == VISIBLE && inChild(c, event.getX() + getScrollX(), event.getY() + getScrollY(), local)) {
                MotionEvent e = MotionEvent.obtain(event);
                e.offsetLocation(local[0] - event.getX(), local[1] - event.getY());
                if (c.dispatchHoverEvent(e)) return true;
            }
        }
        return super.dispatchHoverEvent(event);
    }

    // ---- keys and focus ----
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (focusedChild != null && focusedChild.dispatchKeyEvent(event)) return true;
        return isFocused() && super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchKeyEventPreIme(KeyEvent event) {
        return focusedChild != null ? focusedChild.dispatchKeyEventPreIme(event) : super.dispatchKeyEventPreIme(event);
    }
    @Override public boolean dispatchKeyShortcutEvent(KeyEvent event) {
        return focusedChild != null ? focusedChild.dispatchKeyShortcutEvent(event) : super.dispatchKeyShortcutEvent(event);
    }
    @Override boolean dispatchUnhandledKey(KeyEvent event) {
        if (focusedChild != null && focusedChild.dispatchUnhandledKey(event)) return true;
        return super.dispatchUnhandledKey(event);
    }
    @Override public void requestChildFocus(View child, View focused) {
        if (descendantFocusability == FOCUS_BLOCK_DESCENDANTS) return;
        unFocus();
        if (focusedChild != null && focusedChild != child) focusedChild.unFocus();
        focusedChild = child;
        if (getParent() != null) getParent().requestChildFocus(this, focused);
    }
    @Override public void clearChildFocus(View child) {
        focusedChild = null;
        if (getParent() != null) getParent().clearChildFocus(this);
    }
    @Override void unFocus() {
        if (focusedChild != null) { focusedChild.unFocus(); focusedChild = null; }
        super.unFocus();
    }
    @Override public void clearFocus() {
        if (focusedChild != null) { View c = focusedChild; c.clearFocus(); }
        else super.clearFocus();
    }
    public View getFocusedChild() { return focusedChild; }
    @Override public boolean hasFocus() { return isFocused() || focusedChild != null; }
    @Override public View findFocus() {
        if (isFocused()) return this;
        return focusedChild != null ? focusedChild.findFocus() : null;
    }
    @Override public boolean hasFocusable() {
        if (getVisibility() != VISIBLE) return false;
        if (isFocusable()) return true;
        for (View c : children) if (c.hasFocusable()) return true;
        return false;
    }
    @Override public void addFocusables(ArrayList<View> views, int direction, int focusableMode) {
        if (descendantFocusability != FOCUS_BLOCK_DESCENDANTS) for (View c : children) if (c.getVisibility() == VISIBLE) c.addFocusables(views, direction, focusableMode);
        super.addFocusables(views, direction, focusableMode);
    }
    @Override public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        if (descendantFocusability == FOCUS_BLOCK_DESCENDANTS) return super.requestFocus(direction, previouslyFocusedRect);
        if (descendantFocusability == FOCUS_BEFORE_DESCENDANTS && super.requestFocus(direction, previouslyFocusedRect)) return true;
        if (onRequestFocusInDescendants(direction, previouslyFocusedRect)) return true;
        return descendantFocusability == FOCUS_AFTER_DESCENDANTS && super.requestFocus(direction, previouslyFocusedRect);
    }
    public int getDescendantFocusability() { return descendantFocusability; }
    public void setDescendantFocusability(int focusability) { descendantFocusability = focusability; }
    @Override public View focusSearch(View focused, int direction) { return getParent() != null ? getParent().focusSearch(focused, direction) : null; }
    @Override public void focusableViewAvailable(View v) { if (getParent() != null) getParent().focusableViewAvailable(v); }
    @Override public boolean showContextMenuForChild(View originalView) { return false; }
    @Override public boolean showContextMenuForChild(View originalView, float x, float y) { return false; }
    @Override public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback) { return null; }
    @Override public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback, int type) { return null; }
    @Override public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) { return false; }
    @Override public boolean requestSendAccessibilityEvent(View child, android.view.accessibility.AccessibilityEvent event) { return false; }
    public boolean onRequestSendAccessibilityEvent(View child, android.view.accessibility.AccessibilityEvent event) {
        AccessibilityDelegate d = getAccessibilityDelegate();
        return d == null || d.onRequestSendAccessibilityEvent(this, child, event);
    }
    /** Gives focus to the first visible child that takes it, in the order of the direction. */
    protected boolean onRequestFocusInDescendants(int direction, Rect previouslyFocusedRect) {
        boolean backward = direction == FOCUS_BACKWARD || direction == FOCUS_UP || direction == FOCUS_LEFT;
        for (int n = children.size(), i = 0; i < n; ++i) {
            View c = children.get(backward ? n - 1 - i : i);
            if (c.getVisibility() == VISIBLE && c.requestFocus(direction, previouslyFocusedRect)) return true;
        }
        return false;
    }
    @Override public void requestTransparentRegion(View child) {}
    @Override public void recomputeViewAttributes(View child) {}
    @Override public boolean getChildVisibleRect(View child, Rect r, android.graphics.Point offset) { return true; }
    @Override public void notifySubtreeAccessibilityStateChanged(View child, View source, int changeType) {}
    @Override public boolean canResolveLayoutDirection() { return true; }
    @Override public boolean isLayoutDirectionResolved() { return true; }
    @Override public int getLayoutDirection() { return super.getLayoutDirection(); }
    @Override public boolean onStartNestedScroll(View child, View target, int nestedScrollAxes) { return false; }
    @Override public void onNestedScrollAccepted(View child, View target, int axes) {}
    @Override public void onStopNestedScroll(View child) {}
    @Override public void onNestedScroll(View target, int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed) {}
    @Override public void onNestedPreScroll(View target, int dx, int dy, int[] consumed) {}
    @Override public boolean onNestedFling(View target, float velocityX, float velocityY, boolean consumed) { return false; }
    @Override public boolean onNestedPreFling(View target, float velocityX, float velocityY) { return false; }
    @Override public boolean onNestedPrePerformAccessibilityAction(View target, int action, android.os.Bundle args) { return false; }
    public int getNestedScrollAxes() { return SCROLL_AXIS_NONE; }

    // ---- insets and state ----
    @Override public WindowInsets dispatchApplyWindowInsets(WindowInsets insets) {
        insets = super.dispatchApplyWindowInsets(insets);
        if (insets.isConsumed()) return insets;
        for (View c : children) c.dispatchApplyWindowInsets(insets);
        return insets;
    }
    @Override protected void dispatchSaveInstanceState(SparseArray<Parcelable> container) {
        super.dispatchSaveInstanceState(container);
        for (View c : children) if (c.isSaveFromParentEnabled()) c.dispatchSaveInstanceState(container);
    }
    @Override protected void dispatchRestoreInstanceState(SparseArray<Parcelable> container) {
        super.dispatchRestoreInstanceState(container);
        for (View c : children) if (c.isSaveFromParentEnabled()) c.dispatchRestoreInstanceState(container);
    }
    @Override public void dispatchProvideStructure(ViewStructure structure) { super.dispatchProvideStructure(structure); }
    @Override public CharSequence getAccessibilityClassName() { return ViewGroup.class.getName(); }
    @Override public void jumpDrawablesToCurrentState() {
        super.jumpDrawablesToCurrentState();
        for (View c : children) c.jumpDrawablesToCurrentState();
    }
}
