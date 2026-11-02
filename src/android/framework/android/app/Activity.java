// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentCallbacks2;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.ActionMode;
import android.view.ContextThemeWrapper;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import java.util.ArrayList;

/**
 * A screen of the app. The runtime (snx.android.app.Activities) makes it, attaches it, and drives its lifecycle in
 * Android's order: the pre and post ActivityLifecycleCallbacks around each step, the step's own from its super call.
 */
public class Activity extends ContextThemeWrapper
        implements LayoutInflater.Factory2, Window.Callback, KeyEvent.Callback, View.OnCreateContextMenuListener, ComponentCallbacks2 {
    public static final int RESULT_CANCELED = 0, RESULT_OK = -1, RESULT_FIRST_USER = 1;
    public static final int DEFAULT_KEYS_DISABLE = 0, DEFAULT_KEYS_DIALER = 1, DEFAULT_KEYS_SHORTCUT = 2,
        DEFAULT_KEYS_SEARCH_LOCAL = 3, DEFAULT_KEYS_SEARCH_GLOBAL = 4;
    public static final int OVERRIDE_TRANSITION_OPEN = 0, OVERRIDE_TRANSITION_CLOSE = 1;
    protected static final int[] FOCUSED_STATE_SET = {0x0101009c};

    private Application application;
    private Intent intent;
    private ActivityInfo info;
    private ComponentName component;
    private Window window;
    private WindowManager windowManager;
    private CharSequence title;
    private boolean called, finished, resumed, stopped = true, destroyed, changingConfigurations, visible = true, windowAdded;
    private Object lastNonConfigurationInstance;
    private final ArrayList<Application.ActivityLifecycleCallbacks> lifecycleCallbacks = new ArrayList<>();
    private FragmentManager fragmentManager;
    private int resultCode = RESULT_CANCELED, requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
    private Intent resultData;
    private Activity parent;
    private final Handler handler = new Handler(Looper.getMainLooper());

    public Activity() { super(null, 0); }

    /** The runtime's attach: the context, the application, the intent and the window. */
    public final void attach(Context base, Application app, Intent intent, ActivityInfo info, WindowManager wm, Object lastNonConfig) {
        attachBaseContext(base);
        application = app;
        this.intent = intent;
        this.info = info;
        component = new ComponentName(info.packageName, info.name);
        title = info.loadLabel(base.getPackageManager());
        lastNonConfigurationInstance = lastNonConfig;
        window = new Window(this);
        window.setCallback(this);
        window.setWindowManager(wm, null, info.name);
        windowManager = wm;
        if (info.softInputMode != 0) window.setSoftInputMode(info.softInputMode);
        ((android.window.OnBackInvokedDispatcher.Default) window.getOnBackInvokedDispatcher()).setFallback(this::onBackPressed);
        fragmentManager = new FragmentManager(this);
        int theme = info.getThemeResource();
        if (theme != 0) setTheme(theme);
    }

    public Intent getIntent() { return intent; }
    public void setIntent(Intent newIntent) { intent = newIntent; }
    public final Application getApplication() { return application; }
    public final boolean isChild() { return parent != null; }
    public final Activity getParent() { return parent; }
    public WindowManager getWindowManager() { return windowManager; }
    public Window getWindow() { return window; }
    public android.window.OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return window.getOnBackInvokedDispatcher(); }
    public ComponentName getComponentName() { return component; }
    public ComponentName getCallingActivity() { return null; }
    public String getCallingPackage() { return null; }
    public android.net.Uri getReferrer() { return null; }
    public int getTaskId() { return 1; }
    public boolean isTaskRoot() { return true; }
    public boolean moveTaskToBack(boolean nonRoot) { return false; }
    public View getCurrentFocus() { return window != null ? window.getCurrentFocus() : null; }
    public FragmentManager getFragmentManager() { return fragmentManager; }
    public ActionBar getActionBar() { return null; }
    public MenuInflater getMenuInflater() { return new MenuInflater(this); }
    public LayoutInflater getLayoutInflater() { return getWindow().getLayoutInflater(); }
    public CharSequence getTitle() { return title; }
    public void setTitle(CharSequence title) { this.title = title; window.setTitle(title); }
    public void setTitle(int titleId) { setTitle(getText(titleId)); }
    public void setTitleColor(int textColor) {}
    public final int getTitleColor() { return 0; }
    public boolean isFinishing() { return finished; }
    public boolean isDestroyed() { return destroyed; }
    public boolean isChangingConfigurations() { return changingConfigurations; }
    public boolean isInMultiWindowMode() { return false; }
    public boolean isInPictureInPictureMode() { return false; }
    public boolean enterPictureInPictureMode(PictureInPictureParams params) { return false; }
    public void setPictureInPictureParams(PictureInPictureParams params) {}
    public boolean isVoiceInteraction() { return false; }
    public boolean isVoiceInteractionRoot() { return false; }
    public boolean isLaunchedFromBubble() { return false; }
    public boolean isActivityTransitionRunning() { return false; }
    public Object getLastNonConfigurationInstance() { return lastNonConfigurationInstance; }
    public Object onRetainNonConfigurationInstance() { return null; }
    public final void runOnUiThread(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) action.run(); else handler.post(action);
    }
    public void setRequestedOrientation(int requestedOrientation) { this.requestedOrientation = requestedOrientation; }
    public int getRequestedOrientation() { return requestedOrientation; }
    public void setVisible(boolean visible) { this.visible = visible; }
    public void setShowWhenLocked(boolean showWhenLocked) {}
    public void setTurnScreenOn(boolean turnScreenOn) {}
    public void setInheritShowWhenLocked(boolean inheritShowWhenLocked) {}
    public void setTaskDescription(ActivityManager.TaskDescription taskDescription) {}
    public void setVolumeControlStream(int streamType) {}
    public void setDefaultKeyMode(int mode) {}
    public void setFinishOnTouchOutside(boolean finish) {}
    public void setLocusContext(Object locusId, Bundle bundle) {}
    public void setRecentsScreenshotEnabled(boolean enabled) {}
    public void setTranslucent(boolean translucent) {}
    public void reportFullyDrawn() {}
    public void overridePendingTransition(int enterAnim, int exitAnim) {}
    public void overridePendingTransition(int enterAnim, int exitAnim, int backgroundColor) {}
    public void overrideActivityTransition(int overrideType, int enterAnim, int exitAnim) {}
    public void overrideActivityTransition(int overrideType, int enterAnim, int exitAnim, int backgroundColor) {}
    public void clearOverrideActivityTransition(int overrideType) {}
    public void postponeEnterTransition() {}
    public void startPostponedEnterTransition() {}
    public void setEnterSharedElementCallback(SharedElementCallback callback) {}
    public void setExitSharedElementCallback(SharedElementCallback callback) {}
    public void recreate() { snx.android.app.Activities.recreate(this); }
    public void registerActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) { synchronized (lifecycleCallbacks) { lifecycleCallbacks.add(callback); } }
    public void unregisterActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) { synchronized (lifecycleCallbacks) { lifecycleCallbacks.remove(callback); } }

    @Override public Object getSystemService(String name) {
        if (WINDOW_SERVICE.equals(name)) return windowManager;
        return super.getSystemService(name);
    }

    // ---- content ----
    public <T extends View> T findViewById(int id) { return getWindow().findViewById(id); }
    public final <T extends View> T requireViewById(int id) {
        T v = findViewById(id);
        if (v == null) throw new IllegalArgumentException("ID does not reference a View inside this Activity");
        return v;
    }
    public void setContentView(int layoutResID) { getWindow().setContentView(layoutResID); }
    public void setContentView(View view) { getWindow().setContentView(view); }
    public void setContentView(View view, ViewGroup.LayoutParams params) { getWindow().setContentView(view, params); }
    public void addContentView(View view, ViewGroup.LayoutParams params) { getWindow().addContentView(view, params); }

    // ---- lifecycle callbacks ----
    private Application.ActivityLifecycleCallbacks[] callbacks() {
        synchronized (lifecycleCallbacks) { return lifecycleCallbacks.toArray(new Application.ActivityLifecycleCallbacks[0]); }
    }
    private interface Each { void run(Application.ActivityLifecycleCallbacks c); }
    /** Up the lifecycle: the application's callbacks, then the activity's own. */
    private void up(Each e) {
        if (application != null) for (Application.ActivityLifecycleCallbacks c : application.activityCallbacks()) e.run(c);
        for (Application.ActivityLifecycleCallbacks c : callbacks()) e.run(c);
    }
    /** Down the lifecycle: the activity's own in reverse, then the application's. */
    private void down(Each e) {
        Application.ActivityLifecycleCallbacks[] own = callbacks();
        for (int i = own.length - 1; i >= 0; --i) e.run(own[i]);
        if (application != null) {
            Application.ActivityLifecycleCallbacks[] app = application.activityCallbacks();
            for (int i = app.length - 1; i >= 0; --i) e.run(app[i]);
        }
    }

    private void check(String what) {
        if (!called) throw new SuperNotCalledException("Activity " + component.toShortString() + " did not call through to super." + what + "()");
    }

    // ---- the runtime's lifecycle steps ----
    public final void performCreate(Bundle icicle) {
        up(c -> c.onActivityPreCreated(this, icicle));
        called = false;
        onCreate(icicle);
        check("onCreate");
        fragmentManager.dispatchCreate();
        up(c -> c.onActivityPostCreated(this, icicle));
    }
    public final void performStart() {
        up(c -> c.onActivityPreStarted(this));
        called = false;
        onStart();
        check("onStart");
        stopped = false;
        fragmentManager.dispatchStart();
        up(c -> c.onActivityPostStarted(this));
    }
    public final void performRestoreInstanceState(Bundle state) { onRestoreInstanceState(state); }
    public final void performPostCreate(Bundle icicle) {
        called = false;
        onPostCreate(icicle);
        check("onPostCreate");
    }
    public final void performResume() {
        up(c -> c.onActivityPreResumed(this));
        called = false;
        onResume();
        check("onResume");
        resumed = true;
        fragmentManager.dispatchResume();
        called = false;
        onPostResume();
        check("onPostResume");
        up(c -> c.onActivityPostResumed(this));
    }
    /** The window shown, after the first resume, as Android's makeVisible does. */
    public final void makeVisible() {
        if (windowAdded || !visible) return;
        windowAdded = true;
        windowManager.addView(window.getDecorView(), window.getAttributes());
    }
    public final void performPause() {
        down(c -> c.onActivityPrePaused(this));
        fragmentManager.dispatchPause();
        called = false;
        onPause();
        check("onPause");
        resumed = false;
        down(c -> c.onActivityPostPaused(this));
    }
    public final void performSaveInstanceState(Bundle outState) {
        down(c -> c.onActivityPreSaveInstanceState(this, outState));
        onSaveInstanceState(outState);
        down(c -> c.onActivityPostSaveInstanceState(this, outState));
    }
    public final void performStop() {
        down(c -> c.onActivityPreStopped(this));
        fragmentManager.dispatchStop();
        called = false;
        onStop();
        check("onStop");
        stopped = true;
        down(c -> c.onActivityPostStopped(this));
    }
    public final void performDestroy() {
        down(c -> c.onActivityPreDestroyed(this));
        destroyed = true;
        fragmentManager.dispatchDestroy();
        called = false;
        onDestroy();
        check("onDestroy");
        if (windowAdded) windowManager.removeViewImmediate(window.getDecorView());
        down(c -> c.onActivityPostDestroyed(this));
    }
    public final void performNewIntent(Intent intent) { onNewIntent(intent); }
    public final void performActivityResult(int requestCode, int resultCode, Intent data) { onActivityResult(requestCode, resultCode, data); }
    public final void setChangingConfigurations(boolean changing) { changingConfigurations = changing; }
    public final int resultCode() { return resultCode; }
    public final Intent resultData() { return resultData; }
    public final ActivityInfo activityInfo() { return info; }

    // ---- the lifecycle the app overrides ----
    protected void onCreate(Bundle savedInstanceState) {
        if (savedInstanceState != null) {
            Bundle w = savedInstanceState.getBundle("android:viewHierarchyState");
            if (w != null) getWindow().restoreHierarchyState(w);
        }
        up(c -> c.onActivityCreated(this, savedInstanceState));
        called = true;
    }
    public void onCreate(Bundle savedInstanceState, android.os.PersistableBundle persistentState) { onCreate(savedInstanceState); }
    protected void onRestoreInstanceState(Bundle savedInstanceState) {}
    public void onRestoreInstanceState(Bundle savedInstanceState, android.os.PersistableBundle persistentState) { onRestoreInstanceState(savedInstanceState); }
    protected void onPostCreate(Bundle savedInstanceState) { called = true; }
    public void onPostCreate(Bundle savedInstanceState, android.os.PersistableBundle persistentState) { onPostCreate(savedInstanceState); }
    protected void onStart() { up(c -> c.onActivityStarted(this)); called = true; }
    protected void onRestart() { called = true; }
    public void onStateNotSaved() {}
    protected void onResume() { up(c -> c.onActivityResumed(this)); called = true; }
    protected void onPostResume() { called = true; }
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {}
    protected void onNewIntent(Intent intent) {}
    protected void onSaveInstanceState(Bundle outState) {
        outState.putBundle("android:viewHierarchyState", getWindow().saveHierarchyState());
        down(c -> c.onActivitySaveInstanceState(this, outState));
    }
    public void onSaveInstanceState(Bundle outState, android.os.PersistableBundle outPersistentState) { onSaveInstanceState(outState); }
    protected void onPause() { down(c -> c.onActivityPaused(this)); called = true; }
    protected void onUserLeaveHint() {}
    protected void onStop() { down(c -> c.onActivityStopped(this)); called = true; }
    protected void onDestroy() { down(c -> c.onActivityDestroyed(this)); called = true; }
    @Override public void onConfigurationChanged(Configuration newConfig) { called = true; }
    @Override public void onLowMemory() { called = true; }
    @Override public void onTrimMemory(int level) { called = true; }
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) { onMultiWindowModeChanged(isInMultiWindowMode); }
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode) {}
    public void onPictureInPictureModeChanged(boolean isInPictureInPictureMode, Configuration newConfig) { onPictureInPictureModeChanged(isInPictureInPictureMode); }
    public void onPictureInPictureModeChanged(boolean isInPictureInPictureMode) {}
    public boolean onPictureInPictureRequested() { return false; }
    public void onProvideAssistData(Bundle data) {}
    public void onEnterAnimationComplete() {}
    public void onLocalVoiceInteractionStarted() {}
    public void onLocalVoiceInteractionStopped() {}

    // ---- finishing and results ----
    public void finish() {
        if (finished) return;
        finished = true;
        snx.android.app.Activities.finish(this);
    }
    public void finishAffinity() { finish(); }
    public void finishAfterTransition() { finish(); }
    public void finishAndRemoveTask() { finish(); }
    public void finishActivity(int requestCode) {}
    public final void setResult(int resultCode) { this.resultCode = resultCode; }
    public final void setResult(int resultCode, Intent data) { this.resultCode = resultCode; resultData = data; }
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {}
    public void onActivityReenter(int resultCode, Intent data) {}

    // ---- starting things ----
    @Override public void startActivity(Intent intent) { startActivity(intent, null); }
    @Override public void startActivity(Intent intent, Bundle options) { startActivityForResult(intent, -1, options); }
    public void startActivityForResult(Intent intent, int requestCode) { startActivityForResult(intent, requestCode, null); }
    public void startActivityForResult(Intent intent, int requestCode, Bundle options) {
        snx.android.app.Activities.start(this, intent, requestCode, options);
    }
    public boolean startActivityIfNeeded(Intent intent, int requestCode) { startActivityForResult(intent, requestCode); return true; }
    public void startIntentSenderForResult(IntentSender intent, int requestCode, Intent fillInIntent, int flagsMask, int flagsValues,
                                           int extraFlags, Bundle options) throws IntentSender.SendIntentException {
        throw new IntentSender.SendIntentException("snx: no intent senders");
    }
    public void startIntentSenderForResult(IntentSender intent, int requestCode, Intent fillInIntent, int flagsMask, int flagsValues,
                                           int extraFlags) throws IntentSender.SendIntentException {
        startIntentSenderForResult(intent, requestCode, fillInIntent, flagsMask, flagsValues, extraFlags, null);
    }

    // ---- permissions: every one is granted ----
    public final void requestPermissions(String[] permissions, int requestCode) {
        int[] results = new int[permissions.length];
        handler.post(() -> onRequestPermissionsResult(requestCode, permissions, results));
    }
    public final void requestPermissions(String[] permissions, int requestCode, int deviceId) { requestPermissions(permissions, requestCode); }
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {}
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults, int deviceId) {
        onRequestPermissionsResult(requestCode, permissions, grantResults);
    }
    public boolean shouldShowRequestPermissionRationale(String permission) { return false; }
    public boolean shouldShowRequestPermissionRationale(String permission, int deviceId) { return false; }

    // ---- input: Window.Callback and KeyEvent.Callback ----
    public void onUserInteraction() {}
    @Override public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev.getAction() == MotionEvent.ACTION_DOWN) onUserInteraction();
        if (getWindow().superDispatchTouchEvent(ev)) return true;
        return onTouchEvent(ev);
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        onUserInteraction();
        if (getWindow().superDispatchKeyEvent(event)) return true;
        View decor = getWindow().getDecorView();
        return event.dispatch(this, decor != null ? decor.getKeyDispatcherState() : null, this);
    }
    @Override public boolean dispatchKeyShortcutEvent(KeyEvent event) {
        onUserInteraction();
        return getWindow().superDispatchKeyShortcutEvent(event) || onKeyShortcut(event.getKeyCode(), event);
    }
    @Override public boolean dispatchTrackballEvent(MotionEvent ev) { return onTrackballEvent(ev); }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        onUserInteraction();
        return getWindow().superDispatchGenericMotionEvent(ev) || onGenericMotionEvent(ev);
    }
    @Override public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) { return false; }
    public boolean onTouchEvent(MotionEvent event) { return false; }
    public boolean onTrackballEvent(MotionEvent event) { return false; }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
    public boolean onKeyShortcut(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) { event.startTracking(); return true; }
        return false;
    }
    @Override public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && !event.isCanceled()) {
            android.window.OnBackInvokedDispatcher d = getOnBackInvokedDispatcher();
            if (d instanceof android.window.OnBackInvokedDispatcher.Default dd) dd.invoke(); else onBackPressed();
            return true;
        }
        return false;
    }
    @Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    @Deprecated public void onBackPressed() {
        if (fragmentManager.popBackStackImmediate()) return;
        finishAfterTransition();
    }
    public boolean onNavigateUp() { finish(); return true; }
    @Override public boolean onSearchRequested() { return false; }
    @Override public boolean onSearchRequested(SearchEvent searchEvent) { return false; }
    public void onPointerCaptureChanged(boolean hasCapture) {}

    // ---- the rest of Window.Callback ----
    @Override public View onCreatePanelView(int featureId) { return null; }
    @Override public boolean onCreatePanelMenu(int featureId, Menu menu) { return featureId == Window.FEATURE_OPTIONS_PANEL && onCreateOptionsMenu(menu); }
    @Override public boolean onPreparePanel(int featureId, View view, Menu menu) { return featureId == Window.FEATURE_OPTIONS_PANEL && onPrepareOptionsMenu(menu); }
    @Override public boolean onMenuOpened(int featureId, Menu menu) { return true; }
    @Override public boolean onMenuItemSelected(int featureId, MenuItem item) { return featureId == Window.FEATURE_OPTIONS_PANEL && onOptionsItemSelected(item); }
    @Override public void onPanelClosed(int featureId, Menu menu) {}
    public boolean onCreateOptionsMenu(Menu menu) { return true; }
    public boolean onPrepareOptionsMenu(Menu menu) { return true; }
    public boolean onOptionsItemSelected(MenuItem item) { return false; }
    public void invalidateOptionsMenu() {}
    public void openOptionsMenu() {}
    public void closeOptionsMenu() {}
    @Override public void onWindowAttributesChanged(WindowManager.LayoutParams params) {}
    @Override public void onContentChanged() {}
    @Override public void onWindowFocusChanged(boolean hasFocus) {}
    @Override public void onAttachedToWindow() {}
    @Override public void onDetachedFromWindow() {}
    @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback callback) { return null; }
    @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int type) { return null; }
    @Override public void onActionModeStarted(ActionMode mode) {}
    @Override public void onActionModeFinished(ActionMode mode) {}
    public ActionMode startActionMode(ActionMode.Callback callback) { return null; }
    public ActionMode startActionMode(ActionMode.Callback callback, int type) { return null; }
    @Override public void onCreateContextMenu(android.view.ContextMenu menu, View v, android.view.ContextMenu.ContextMenuInfo menuInfo) {}
    public void registerForContextMenu(View view) {}
    public void unregisterForContextMenu(View view) {}

    // ---- LayoutInflater.Factory2 ----
    @Override public View onCreateView(String name, Context context, AttributeSet attrs) { return null; }
    @Override public View onCreateView(View parent, String name, Context context, AttributeSet attrs) { return null; }

    @Override protected void onApplyThemeResource(Resources.Theme theme, int resid, boolean first) { super.onApplyThemeResource(theme, resid, first); }
    public final boolean isResumed() { return resumed; }
}
