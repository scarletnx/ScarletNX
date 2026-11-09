// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.RenderNode;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import snx.android.ui.Host;

/**
 * A window's root: the parent of its decor view. Its traversal measures, lays out and records the tree, and the
 * host draws the recording; input from the host comes in here. Android's ViewRootImpl without a WindowManager
 * service, a surface or a render thread.
 */
public final class ViewRootImpl implements ViewParent {
    private final Context context;
    private View view;
    private View.AttachInfo attachInfo;
    private final Handler handler;
    private int width, height;
    private boolean traversalScheduled, layoutRequested = true, first = true, dead;
    private final KeyEvent.DispatcherState keyState = new KeyEvent.DispatcherState();
    private final WindowInsetsController insetsController = new WindowInsetsController.Simple();
    private android.window.OnBackInvokedDispatcher backDispatcher;
    private WindowManager.LayoutParams attributes;
    private View focusBeforeTouch;

    public ViewRootImpl(Context context) {
        this.context = context;
        handler = new Handler(Looper.myLooper());
    }

    public void setView(View view, WindowManager.LayoutParams attrs, android.window.OnBackInvokedDispatcher dispatcher) {
        this.view = view;
        attributes = attrs;
        backDispatcher = dispatcher != null ? dispatcher : new android.window.OnBackInvokedDispatcher.Default();
        attachInfo = new View.AttachInfo(this, handler, new Binder());
        attachInfo.rootView = view;
        attachInfo.hasWindowFocus = true;
        android.util.DisplayMetrics m = snx.android.Device.metrics();
        if (offscreen()) {
            android.graphics.Point size = new android.graphics.Point();
            display.getSize(size);
            m = new android.util.DisplayMetrics();
            m.widthPixels = size.x;
            m.heightPixels = size.y;
        }
        width = attrs != null && attrs.width > 0 ? attrs.width : m.widthPixels;
        height = attrs != null && attrs.height > 0 ? attrs.height : m.heightPixels;
        view.mParent = this;
        if (!offscreen()) Host.attach(this);
        requestLayout();
    }

    /* the display the window is on: one other than the host window's (a virtual display) lays its views out and draws
       nothing anywhere, as what such a display shows goes to no screen */
    private Display display = Display.DEFAULT;
    public void setDisplay(Display display) { this.display = display != null ? display : Display.DEFAULT; }
    boolean offscreen() { return display.getDisplayId() != Display.DEFAULT_DISPLAY; }

    public View getView() { return view; }
    public void die() {
        dead = true;
        if (view != null && view.mAttachInfo != null) view.dispatchDetachedFromWindow();
        if (!offscreen()) Host.detach(this);
    }
    /* the window's place in the display: its root view is laid out there, so that it draws there; events come
       into the window's own coordinates */
    private int windowLeft, windowTop;
    int windowX() { return 0; }
    int windowY() { return 0; }
    private static int windowSpec(int param, int display) {
        if (param > 0) return View.MeasureSpec.makeMeasureSpec(Math.min(param, display), View.MeasureSpec.EXACTLY);
        if (param == ViewGroup.LayoutParams.WRAP_CONTENT) return View.MeasureSpec.makeMeasureSpec(display, View.MeasureSpec.AT_MOST);
        return View.MeasureSpec.makeMeasureSpec(display, View.MeasureSpec.EXACTLY);
    }
    KeyEvent.DispatcherState keyDispatcherState() { return keyState; }
    WindowInsetsController insetsController() { return insetsController; }
    android.window.OnBackInvokedDispatcher onBackInvokedDispatcher() { return backDispatcher; }
    public int width() { return width; }
    public int height() { return height; }

    /** The host window's new size: laid out again. */
    public void resize(int width, int height) {
        if (this.width == width && this.height == height) return;
        this.width = width;
        this.height = height;
        view.forceLayout();
        requestLayout();
    }

    void requestApplyInsets() { first = true; scheduleTraversals(); }

    // ---- ViewParent ----
    @Override public void requestLayout() { layoutRequested = true; scheduleTraversals(); }
    @Override public boolean isLayoutRequested() { return layoutRequested; }
    @Override public void onDescendantInvalidated(View child, View target) { scheduleTraversals(); }
    @Override public void invalidateChild(View child, Rect r) { scheduleTraversals(); }
    @Override public ViewParent invalidateChildInParent(int[] location, Rect r) { scheduleTraversals(); return null; }
    @Override public ViewParent getParent() { return null; }
    @Override public void requestTransparentRegion(View child) {}
    @Override public void requestChildFocus(View child, View focused) {}
    @Override public void recomputeViewAttributes(View child) {}
    @Override public void clearChildFocus(View child) {}
    @Override public boolean getChildVisibleRect(View child, Rect r, android.graphics.Point offset) { return r.intersect(0, 0, width, height); }
    @Override public View focusSearch(View focused, int direction) {
        return view instanceof ViewGroup g ? FocusFinder.getInstance().findNextFocus(g, focused, direction) : null;
    }
    @Override public void bringChildToFront(View child) {}
    @Override public void focusableViewAvailable(View v) {}
    @Override public boolean showContextMenuForChild(View originalView) { return false; }
    @Override public boolean showContextMenuForChild(View originalView, float x, float y) { return false; }
    @Override public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback) { return null; }
    @Override public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback, int type) { return null; }
    @Override public void childDrawableStateChanged(View child) {}
    @Override public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {}
    @Override public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) { return false; }
    @Override public boolean requestSendAccessibilityEvent(View child, android.view.accessibility.AccessibilityEvent event) { return false; }
    @Override public void childHasTransientStateChanged(View child, boolean hasTransientState) {}
    @Override public void notifySubtreeAccessibilityStateChanged(View child, View source, int changeType) {}
    @Override public boolean canResolveLayoutDirection() { return true; }
    @Override public boolean isLayoutDirectionResolved() { return true; }
    @Override public int getLayoutDirection() { return View.LAYOUT_DIRECTION_LTR; }
    @Override public boolean onStartNestedScroll(View child, View target, int nestedScrollAxes) { return false; }
    @Override public void onNestedScrollAccepted(View child, View target, int nestedScrollAxes) {}
    @Override public void onStopNestedScroll(View target) {}
    @Override public void onNestedScroll(View target, int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed) {}
    @Override public void onNestedPreScroll(View target, int dx, int dy, int[] consumed) {}
    @Override public boolean onNestedFling(View target, float velocityX, float velocityY, boolean consumed) { return false; }
    @Override public boolean onNestedPreFling(View target, float velocityX, float velocityY) { return false; }
    @Override public boolean onNestedPrePerformAccessibilityAction(View target, int action, android.os.Bundle args) { return false; }

    // ---- traversals ----
    void scheduleTraversals() {
        if (traversalScheduled || dead) return;
        traversalScheduled = true;
        Choreographer.getInstance().postCallback(Choreographer.CALLBACK_TRAVERSAL, this::doTraversal, null);
    }

    private void doTraversal() {
        traversalScheduled = false;
        if (dead || view == null) return;
        if (first) {
            first = false;
            view.dispatchAttachedToWindow(attachInfo, View.VISIBLE);
            attachInfo.treeObserver.dispatchOnWindowAttachedChange(true);
            view.dispatchApplyWindowInsets(WindowInsets.none());
            view.dispatchWindowFocusChanged(true);
            attachInfo.treeObserver.dispatchOnWindowFocusChange(true);
        }
        if (layoutRequested || view.isLayoutRequested()) {
            layoutRequested = false;
            // the window's size by its params (a wrapping one at most the display's), and its place by its gravity,
            // as Android's window manager lays a window out: a floating one without a gravity in the middle
            view.measure(windowSpec(attributes != null ? attributes.width : ViewGroup.LayoutParams.MATCH_PARENT, width),
                windowSpec(attributes != null ? attributes.height : ViewGroup.LayoutParams.MATCH_PARENT, height));
            int w = view.getMeasuredWidth(), h = view.getMeasuredHeight();
            int gravity = attributes != null && attributes.gravity != 0 ? attributes.gravity : Gravity.CENTER;
            android.graphics.Rect frame = new android.graphics.Rect();
            Gravity.apply(gravity, w, h, new android.graphics.Rect(0, 0, width, height),
                attributes != null ? attributes.x : 0, attributes != null ? attributes.y : 0, frame);
            windowLeft = w >= width ? 0 : frame.left;
            windowTop = h >= height ? 0 : frame.top;
            view.layout(windowLeft, windowTop, windowLeft + w, windowTop + h);
            attachInfo.treeObserver.dispatchOnGlobalLayout();
        }
        if (attachInfo.treeObserver.dispatchOnPreDraw()) {
            scheduleTraversals();
            return;
        }
        attachInfo.drawingTime = SystemClock.uptimeMillis();
        RenderNode root = view.updateDisplayListIfDirty();
        attachInfo.treeObserver.dispatchOnDraw();
        if (!offscreen()) Host.draw(this, root);
    }

    // ---- input ----

    /** An input event from the host window, on the main thread. */
    public boolean dispatchInputEvent(InputEvent event) {
        if (dead || view == null || view.mAttachInfo == null) return false;
        /* a NativeActivity's window: the events go to its native input queue, as Android's input dispatcher sends them */
        if (view instanceof Window.DecorView d && d.window().inputQueue() != null) return d.window().inputQueue().send(event);
        if (event instanceof MotionEvent m0) {
            MotionEvent m = m0;
            if (windowLeft != 0 || windowTop != 0) {
                m = MotionEvent.obtain(m0);
                m.offsetLocation(-windowLeft, -windowTop);
            }
            int action = m.getActionMasked();
            if (action == MotionEvent.ACTION_HOVER_MOVE || action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_EXIT)
                return view.dispatchHoverEvent(m);
            if (action == MotionEvent.ACTION_SCROLL) return view.dispatchGenericMotionEvent(m);
            return view.dispatchTouchEvent(m);
        }
        if (event instanceof KeyEvent k) {
            if (view.dispatchKeyEventPreIme(k)) return true;
            if (view.dispatchKeyEvent(k)) return true;
            if (view.dispatchUnhandledKey(k)) return true;
            if (k.getKeyCode() == KeyEvent.KEYCODE_BACK && k.getAction() == KeyEvent.ACTION_UP) {
                return backDispatcher instanceof android.window.OnBackInvokedDispatcher.Default d && d.invoke();
            }
        }
        return false;
    }

    @Override public String toString() { return "ViewRootImpl{" + view + "}"; }
}
