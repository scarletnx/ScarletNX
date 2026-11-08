// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.ViewTreeObserver;

/**
 * An activity of native code: it loads the library that its meta-data names and calls its ANativeActivity_onCreate
 * (or the function that android.app.func_name names) with an ANativeActivity, then the callbacks that the library set
 * at each step of the activity's life. It takes its window's input queue, which the system's touches and keys then go
 * to, as Android's does. Only the 32-bit native bridge has the glue yet; with another, the library is loaded and its
 * entry point does not run. The window's surface is the views', so the surface callbacks do not run.
 */
public class NativeActivity extends Activity implements SurfaceHolder.Callback2, ViewTreeObserver.OnGlobalLayoutListener,
                                                       android.view.InputQueue.Callback {
    public static final String META_DATA_LIB_NAME = "android.app.lib_name", META_DATA_FUNC_NAME = "android.app.func_name";
    /* ANativeActivityCallbacks' order */
    private static final int ON_START = 0, ON_RESUME = 1, ON_PAUSE = 3, ON_STOP = 4, ON_DESTROY = 5,
        ON_WINDOW_FOCUS_CHANGED = 6, ON_INPUT_QUEUE_CREATED = 11, ON_INPUT_QUEUE_DESTROYED = 12,
        ON_CONFIGURATION_CHANGED = 14, ON_LOW_MEMORY = 15;

    private long activity; /* the native ANativeActivity, 0 without the glue */

    public NativeActivity() {}
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Bundle meta = activityInfo().metaData;
        String lib = meta != null ? meta.getString(META_DATA_LIB_NAME, "main") : "main";
        String func = meta != null ? meta.getString(META_DATA_FUNC_NAME, "ANativeActivity_onCreate") : "ANativeActivity_onCreate";
        snx.dalvik.Runtime.loadLibrary(lib);
        java.io.File external = getExternalFilesDir(null);
        activity = snx.android.nativebridge.NativeBridge.activityCreate(this, func, getFilesDir().getPath(),
            external != null ? external.getPath() : null, android.os.Build.VERSION.SDK_INT);
        if (activity == 0) {
            android.util.Log.w("NativeActivity", "ANativeActivity is not supported: lib" + lib + ".so is loaded, its entry point does not run");
            return;
        }
        getWindow().takeInputQueue(this);
    }
    private void call(int callback, long argument) {
        if (activity != 0) snx.android.nativebridge.NativeBridge.activityCall(activity, callback, argument);
    }
    @Override protected void onStart() { super.onStart(); call(ON_START, 0); }
    @Override protected void onResume() { super.onResume(); call(ON_RESUME, 0); }
    @Override protected void onPause() { call(ON_PAUSE, 0); super.onPause(); }
    @Override protected void onStop() { call(ON_STOP, 0); super.onStop(); }
    @Override protected void onDestroy() {
        getWindow().takeInputQueue(null);
        call(ON_DESTROY, 0);
        super.onDestroy();
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) { super.onWindowFocusChanged(hasFocus); call(ON_WINDOW_FOCUS_CHANGED, hasFocus ? 1 : 0); }
    @Override public void onConfigurationChanged(android.content.res.Configuration newConfig) { super.onConfigurationChanged(newConfig); call(ON_CONFIGURATION_CHANGED, 0); }
    @Override public void onLowMemory() { super.onLowMemory(); call(ON_LOW_MEMORY, 0); }
    @Override public void onInputQueueCreated(android.view.InputQueue queue) { call(ON_INPUT_QUEUE_CREATED, queue.handle()); }
    @Override public void onInputQueueDestroyed(android.view.InputQueue queue) { call(ON_INPUT_QUEUE_DESTROYED, queue.handle()); }
    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
    @Override public void surfaceRedrawNeeded(SurfaceHolder holder) {}
    @Override public void surfaceDestroyed(SurfaceHolder holder) {}
    @Override public void onGlobalLayout() {}
}
