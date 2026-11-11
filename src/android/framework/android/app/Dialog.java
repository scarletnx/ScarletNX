// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.view.ActionMode;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;

/** A window over its activity's. */
public class Dialog implements DialogInterface, Window.Callback, KeyEvent.Callback {
    private final Context context;
    private final Window window;
    private final WindowManager windowManager;
    private boolean created, showing, cancelable = true, canceledOnTouchOutside = true;
    /*
     * The listeners as Android's Dialog keeps them: each a message to the listeners' handler, sent (a copy) when the
     * dialog shows, is cancelled or is dismissed, so that a listener runs after what sent it, as a message of its
     * own. setDismissMessage replaces the dismiss listener's
     */
    private static final int DISMISS = 0x43, CANCEL = 0x44, SHOW = 0x45;
    private final android.os.Handler listeners = new android.os.Handler(android.os.Looper.getMainLooper()) {
        @Override public void handleMessage(android.os.Message msg) {
            switch (msg.what) {
                case DISMISS -> ((OnDismissListener) msg.obj).onDismiss(Dialog.this);
                case CANCEL -> ((OnCancelListener) msg.obj).onCancel(Dialog.this);
                case SHOW -> ((OnShowListener) msg.obj).onShow(Dialog.this);
                default -> {}
            }
        }
    };
    private android.os.Message cancelMessage, showMessage;
    private OnKeyListener onKey;
    private Activity owner;
    private android.os.Message dismissMessage;

    public Dialog(Context context) { this(context, 0); }
    public Dialog(Context context, int themeResId) {
        this.context = themeResId != 0 ? new android.view.ContextThemeWrapper(context, themeResId) : context;
        windowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        window = new Window(this.context);
        window.setCallback(this);
        window.setWindowManager(windowManager, null, null);
        WindowManager.LayoutParams a = window.getAttributes();
        a.width = ViewGroup.LayoutParams.WRAP_CONTENT;
        a.height = ViewGroup.LayoutParams.WRAP_CONTENT;
        a.type = WindowManager.LayoutParams.TYPE_APPLICATION_ATTACHED_DIALOG;
        for (Context c = context; owner == null && c != null; c = c instanceof android.content.ContextWrapper w ? w.getBaseContext() : null)
            if (c instanceof Activity activity) owner = activity;
        /* a dialog of an activity is a window of the activity's token, which shows above the activity's own window */
        if (owner != null && owner.getWindow() != null) a.token = owner.getWindow().getAttributes().token;
        ((android.window.OnBackInvokedDispatcher.Default) window.getOnBackInvokedDispatcher()).setFallback(this::onBackPressed);
    }
    protected Dialog(Context context, boolean cancelable, OnCancelListener cancelListener) {
        this(context);
        this.cancelable = cancelable;
        setOnCancelListener(cancelListener);
    }

    public final Context getContext() { return context; }
    public final Activity getOwnerActivity() { return owner; }
    public final void setOwnerActivity(Activity activity) { owner = activity; }
    public Window getWindow() { return window; }
    public LayoutInflater getLayoutInflater() { return window.getLayoutInflater(); }
    public android.window.OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return window.getOnBackInvokedDispatcher(); }
    public boolean isShowing() { return showing; }
    public View getCurrentFocus() { return window.getCurrentFocus(); }
    public <T extends View> T findViewById(int id) { return window.findViewById(id); }
    public final <T extends View> T requireViewById(int id) { return window.requireViewById(id); }
    public void setContentView(int layoutResID) { window.setContentView(layoutResID); }
    public void setContentView(View view) { window.setContentView(view); }
    public void setContentView(View view, ViewGroup.LayoutParams params) { window.setContentView(view, params); }
    public void addContentView(View view, ViewGroup.LayoutParams params) { window.addContentView(view, params); }
    public void setTitle(CharSequence title) { window.setTitle(title); }
    public void setTitle(int titleId) { setTitle(context.getText(titleId)); }
    public void setCancelable(boolean flag) { cancelable = flag; }
    public void setCanceledOnTouchOutside(boolean cancel) { canceledOnTouchOutside = cancel; }
    public void setOnCancelListener(OnCancelListener listener) { cancelMessage = listener != null ? listeners.obtainMessage(CANCEL, listener) : null; }
    public void setOnDismissListener(OnDismissListener listener) { dismissMessage = listener != null ? listeners.obtainMessage(DISMISS, listener) : null; }
    public void setOnShowListener(OnShowListener listener) { showMessage = listener != null ? listeners.obtainMessage(SHOW, listener) : null; }
    public void setCancelMessage(android.os.Message msg) { cancelMessage = msg; }
    public void setOnKeyListener(OnKeyListener listener) { onKey = listener; }
    /** A message that dismiss() sends, once for each dismiss, as Android sends a copy of it. */
    public void setDismissMessage(android.os.Message msg) { dismissMessage = msg; }
    public final boolean requestWindowFeature(int featureId) { return window.requestFeature(featureId); }
    public void create() { if (!created) { created = true; onCreate(null); } }
    protected void onCreate(Bundle savedInstanceState) {}
    protected void onStart() {}
    protected void onStop() {}
    public Bundle onSaveInstanceState() { return new Bundle(); }
    public void onRestoreInstanceState(Bundle savedInstanceState) {}
    public void show() {
        if (showing) { window.getDecorView().setVisibility(View.VISIBLE); return; }
        create();
        onStart();
        windowManager.addView(window.getDecorView(), window.getAttributes());
        showing = true;
        if (showMessage != null) android.os.Message.obtain(showMessage).sendToTarget();
    }
    public void hide() { window.getDecorView().setVisibility(View.GONE); }
    @Override public void dismiss() {
        if (!showing) return;
        windowManager.removeViewImmediate(window.getDecorView());
        showing = false;
        onStop();
        if (dismissMessage != null) android.os.Message.obtain(dismissMessage).sendToTarget();
    }
    @Override public void cancel() {
        if (cancelMessage != null) android.os.Message.obtain(cancelMessage).sendToTarget();
        dismiss();
    }
    public void onBackPressed() { if (cancelable) cancel(); }
    public boolean onTouchEvent(MotionEvent event) {
        if (cancelable && canceledOnTouchOutside && event.getAction() == MotionEvent.ACTION_UP) {
            View d = window.getDecorView();
            if (event.getX() < 0 || event.getY() < 0 || event.getX() >= d.getWidth() || event.getY() >= d.getHeight()) { cancel(); return true; }
        }
        return false;
    }
    @Override public boolean dispatchTouchEvent(MotionEvent ev) { return window.superDispatchTouchEvent(ev) || onTouchEvent(ev); }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (onKey != null && onKey.onKey(this, event.getKeyCode(), event)) return true;
        if (window.superDispatchKeyEvent(event)) return true;
        return event.dispatch(this, window.getDecorView().getKeyDispatcherState(), this);
    }
    @Override public boolean dispatchKeyShortcutEvent(KeyEvent event) { return window.superDispatchKeyShortcutEvent(event); }
    @Override public boolean dispatchTrackballEvent(MotionEvent ev) { return false; }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent ev) { return window.superDispatchGenericMotionEvent(ev); }
    @Override public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK || keyCode == KeyEvent.KEYCODE_ESCAPE) { event.startTracking(); return true; }
        return false;
    }
    @Override public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if ((keyCode == KeyEvent.KEYCODE_BACK || keyCode == KeyEvent.KEYCODE_ESCAPE) && !event.isCanceled()) {
            ((android.window.OnBackInvokedDispatcher.Default) getOnBackInvokedDispatcher()).invoke();
            return true;
        }
        return false;
    }
    @Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    @Override public View onCreatePanelView(int featureId) { return null; }
    @Override public boolean onCreatePanelMenu(int featureId, Menu menu) { return false; }
    @Override public boolean onPreparePanel(int featureId, View view, Menu menu) { return false; }
    @Override public boolean onMenuOpened(int featureId, Menu menu) { return false; }
    @Override public boolean onMenuItemSelected(int featureId, MenuItem item) { return false; }
    @Override public void onWindowAttributesChanged(WindowManager.LayoutParams params) {}
    @Override public void onContentChanged() {}
    @Override public void onWindowFocusChanged(boolean hasFocus) {}
    @Override public void onAttachedToWindow() {}
    @Override public void onDetachedFromWindow() {}
    @Override public void onPanelClosed(int featureId, Menu menu) {}
    @Override public boolean onSearchRequested() { return false; }
    @Override public boolean onSearchRequested(SearchEvent searchEvent) { return false; }
    @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback callback) { return null; }
    @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int type) { return null; }
    @Override public void onActionModeStarted(ActionMode mode) {}
    @Override public void onActionModeFinished(ActionMode mode) {}
}
