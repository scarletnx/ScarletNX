// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.view.accessibility.AccessibilityEvent;
import android.widget.FrameLayout;

/**
 * An activity's window: a decor view with the content in it, and the window's attributes. Android's PhoneWindow:
 * no title or action bar, no system bars. The window manager shows the decor as one host window.
 */
public class Window {
    public static final int FEATURE_OPTIONS_PANEL = 0, FEATURE_NO_TITLE = 1, FEATURE_PROGRESS = 2, FEATURE_LEFT_ICON = 3,
        FEATURE_RIGHT_ICON = 4, FEATURE_INDETERMINATE_PROGRESS = 5, FEATURE_CONTEXT_MENU = 6, FEATURE_CUSTOM_TITLE = 7,
        FEATURE_ACTION_BAR = 8, FEATURE_ACTION_BAR_OVERLAY = 9, FEATURE_ACTION_MODE_OVERLAY = 10, FEATURE_SWIPE_TO_DISMISS = 11,
        FEATURE_CONTENT_TRANSITIONS = 12, FEATURE_ACTIVITY_TRANSITIONS = 13;
    public static final int ID_ANDROID_CONTENT = 0x01020002;
    public static final int DECOR_CAPTION_SHADE_AUTO = 0;

    public interface Callback {
        boolean dispatchKeyEvent(KeyEvent event);
        boolean dispatchKeyShortcutEvent(KeyEvent event);
        boolean dispatchTouchEvent(MotionEvent event);
        boolean dispatchTrackballEvent(MotionEvent event);
        boolean dispatchGenericMotionEvent(MotionEvent event);
        boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event);
        View onCreatePanelView(int featureId);
        boolean onCreatePanelMenu(int featureId, Menu menu);
        boolean onPreparePanel(int featureId, View view, Menu menu);
        boolean onMenuOpened(int featureId, Menu menu);
        boolean onMenuItemSelected(int featureId, MenuItem item);
        void onWindowAttributesChanged(WindowManager.LayoutParams attrs);
        void onContentChanged();
        void onWindowFocusChanged(boolean hasFocus);
        void onAttachedToWindow();
        void onDetachedFromWindow();
        void onPanelClosed(int featureId, Menu menu);
        boolean onSearchRequested();
        boolean onSearchRequested(SearchEvent searchEvent);
        ActionMode onWindowStartingActionMode(ActionMode.Callback callback);
        ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int type);
        void onActionModeStarted(ActionMode mode);
        void onActionModeFinished(ActionMode mode);
        default void onPointerCaptureChanged(boolean hasCapture) {}
        default void onProvideKeyboardShortcuts(java.util.List<?> data, Menu menu, int deviceId) {}
    }

    public interface OnFrameMetricsAvailableListener {
        void onFrameMetricsAvailable(Window window, FrameMetrics frameMetrics, int dropCountSinceLastInvocation);
    }

    private final Context context;
    private Callback callback;
    private final WindowManager.LayoutParams attributes = new WindowManager.LayoutParams();
    private DecorView decor;
    private FrameLayout content;
    private WindowManager windowManager;
    private int statusBarColor, navigationBarColor = 0xff000000;
    private boolean decorFits = true;
    private final WindowInsetsController insetsController = new WindowInsetsController.Simple();
    private final android.window.OnBackInvokedDispatcher backDispatcher = new android.window.OnBackInvokedDispatcher.Default();
    private int features;

    public Window(Context context) { this.context = context; }
    public boolean isWideColorGamut() { return false; }

    /** The decor: the window's root, which sends input to the window's callback first. */
    final class DecorView extends FrameLayout {
        DecorView(Context c) { super(c); setId(View.generateViewId()); }
        @Override public boolean dispatchTouchEvent(MotionEvent ev) {
            return callback != null ? callback.dispatchTouchEvent(ev) : super.dispatchTouchEvent(ev);
        }
        @Override public boolean dispatchKeyEvent(KeyEvent event) {
            return callback != null ? callback.dispatchKeyEvent(event) : super.dispatchKeyEvent(event);
        }
        @Override public boolean dispatchGenericMotionEvent(MotionEvent ev) {
            return callback != null ? callback.dispatchGenericMotionEvent(ev) : super.dispatchGenericMotionEvent(ev);
        }
        boolean superDispatchTouchEvent(MotionEvent ev) { return super.dispatchTouchEvent(ev); }
        boolean superDispatchKeyEvent(KeyEvent ev) { return super.dispatchKeyEvent(ev); }
        boolean superDispatchGenericMotionEvent(MotionEvent ev) { return super.dispatchGenericMotionEvent(ev); }
        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); if (callback != null) callback.onAttachedToWindow(); }
        @Override protected void onDetachedFromWindow() { super.onDetachedFromWindow(); if (callback != null) callback.onDetachedFromWindow(); }
        @Override public void onWindowFocusChanged(boolean hasWindowFocus) {
            super.onWindowFocusChanged(hasWindowFocus);
            if (callback != null) callback.onWindowFocusChanged(hasWindowFocus);
        }
        Window window() { return Window.this; }
    }

    private void installDecor() {
        if (decor != null) return;
        decor = new DecorView(context);
        content = new FrameLayout(context);
        content.setId(ID_ANDROID_CONTENT);
        decor.addView(content, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // the theme's window background, white without one
        int background = 0xffffffff;
        try {
            TypedArray a = context.getTheme().obtainStyledAttributes(new int[] {0x01010054});
            Drawable d = a.hasValue(0) ? a.getDrawable(0) : null;
            a.recycle();
            if (d != null) { decor.setBackground(d); return; }
        } catch (RuntimeException e) {
            // a drawable this does not load yet: the default
        }
        decor.setBackground(new ColorDrawable(background));
    }

    public final Context getContext() { return context; }
    public void setCallback(Callback callback) { this.callback = callback; }
    public final Callback getCallback() { return callback; }
    public View getDecorView() { installDecor(); return decor; }
    public View peekDecorView() { return decor; }
    public <T extends View> T findViewById(int id) { return getDecorView().findViewById(id); }
    public final <T extends View> T requireViewById(int id) {
        T v = findViewById(id);
        if (v == null) throw new IllegalArgumentException("ID does not reference a View inside this Window");
        return v;
    }
    public void setContentView(int layoutResID) {
        installDecor();
        content.removeAllViews();
        getLayoutInflater().inflate(layoutResID, content);
        if (callback != null) callback.onContentChanged();
    }
    public void setContentView(View view) { setContentView(view, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)); }
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        installDecor();
        content.removeAllViews();
        content.addView(view, params);
        if (callback != null) callback.onContentChanged();
    }
    public void addContentView(View view, ViewGroup.LayoutParams params) {
        installDecor();
        content.addView(view, params);
        if (callback != null) callback.onContentChanged();
    }
    public View getCurrentFocus() { return decor != null ? decor.findFocus() : null; }
    public LayoutInflater getLayoutInflater() { return LayoutInflater.from(context); }
    public boolean superDispatchTouchEvent(MotionEvent event) { return decor.superDispatchTouchEvent(event); }
    public boolean superDispatchKeyEvent(KeyEvent event) { return decor.superDispatchKeyEvent(event); }
    public boolean superDispatchKeyShortcutEvent(KeyEvent event) { return decor.dispatchKeyShortcutEvent(event); }
    public boolean superDispatchGenericMotionEvent(MotionEvent event) { return decor.superDispatchGenericMotionEvent(event); }
    public boolean superDispatchTrackballEvent(MotionEvent event) { return false; }
    public void injectInputEvent(InputEvent event) {}
    public void setWindowManager(WindowManager wm, android.os.IBinder appToken, String appName) { windowManager = wm; attributes.token = appToken; }
    public void setWindowManager(WindowManager wm, android.os.IBinder appToken, String appName, boolean hardwareAccelerated) { setWindowManager(wm, appToken, appName); }
    public WindowManager getWindowManager() { return windowManager; }
    public final WindowManager.LayoutParams getAttributes() { return attributes; }
    public void setAttributes(WindowManager.LayoutParams a) { attributes.copyFrom(a); if (callback != null) callback.onWindowAttributesChanged(attributes); }
    public void addFlags(int flags) { setFlags(flags, flags); }
    public void clearFlags(int flags) { setFlags(0, flags); }
    public void setFlags(int flags, int mask) { attributes.flags = (attributes.flags & ~mask) | (flags & mask); if (callback != null) callback.onWindowAttributesChanged(attributes); }
    public void setSoftInputMode(int mode) { attributes.softInputMode = mode; }
    public void setGravity(int gravity) { attributes.gravity = gravity; }
    public void setLayout(int width, int height) { attributes.width = width; attributes.height = height; }
    public void setType(int type) { attributes.type = type; }
    public void setFormat(int format) { attributes.format = format; }
    public void setWindowAnimations(int resId) { attributes.windowAnimations = resId; }
    public void setDimAmount(float amount) { attributes.dimAmount = amount; }
    public void setTitle(CharSequence title) { attributes.setTitle(title); }
    public void setBackgroundDrawable(Drawable drawable) { installDecor(); decor.setBackground(drawable); }
    public void setBackgroundDrawableResource(int resId) { setBackgroundDrawable(context.getDrawable(resId)); }
    public boolean requestFeature(int featureId) { features |= 1 << featureId; return true; }
    public final boolean hasFeature(int feature) { return (features & (1 << feature)) != 0; }
    public boolean isFloating() { return false; }
    public boolean isActive() { return true; }
    public void takeKeyEvents(boolean get) {}
    public void setStatusBarColor(int color) { statusBarColor = color; }
    public int getStatusBarColor() { return statusBarColor; }
    public void setNavigationBarColor(int color) { navigationBarColor = color; }
    public int getNavigationBarColor() { return navigationBarColor; }
    public void setNavigationBarDividerColor(int dividerColor) {}
    public void setStatusBarContrastEnforced(boolean ensureContrast) {}
    public boolean isStatusBarContrastEnforced() { return false; }
    public void setNavigationBarContrastEnforced(boolean enforceContrast) {}
    public boolean isNavigationBarContrastEnforced() { return false; }
    public void setDecorFitsSystemWindows(boolean decorFitsSystemWindows) { decorFits = decorFitsSystemWindows; }
    public boolean decorFitsSystemWindows() { return decorFits; }
    public WindowInsetsController getInsetsController() { return insetsController; }
    public android.window.OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return backDispatcher; }
    public void setSystemGestureExclusionRects(java.util.List<android.graphics.Rect> rects) {}
    public void setHideOverlayWindows(boolean hide) {}
    public void setSustainedPerformanceMode(boolean enable) {}
    public void setColorMode(int colorMode) {}
    public void setPreferMinimalPostProcessing(boolean isPreferred) {}
    public void addOnFrameMetricsAvailableListener(OnFrameMetricsAvailableListener listener, android.os.Handler handler) {}
    public void removeOnFrameMetricsAvailableListener(OnFrameMetricsAvailableListener listener) {}
    public void setAllowEnterTransitionOverlap(boolean allow) {}
    public void setAllowReturnTransitionOverlap(boolean allow) {}
    public void makeActive() {}
    public void setElevation(float elevation) {}
    public void setClipToOutline(boolean clipToOutline) {}
    public void setFrameRateBoostOnTouchEnabled(boolean enabled) {}
    public Bundle saveHierarchyState() {
        Bundle b = new Bundle();
        if (content != null) {
            android.util.SparseArray<android.os.Parcelable> states = new android.util.SparseArray<>();
            content.saveHierarchyState(states);
            b.putSparseParcelableArray("android:views", states);
        }
        return b;
    }
    public void restoreHierarchyState(Bundle savedInstanceState) {
        android.util.SparseArray<android.os.Parcelable> states = savedInstanceState.getSparseParcelableArray("android:views");
        if (states != null && content != null) content.restoreHierarchyState(states);
    }
}
