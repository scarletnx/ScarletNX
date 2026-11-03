// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.ViewTreeObserver;

/**
 * An activity of native code: it loads the library that its meta-data names. The runtime has no ANativeActivity
 * glue yet, so ANativeActivity_onCreate and the native callbacks do not run.
 */
public class NativeActivity extends Activity implements SurfaceHolder.Callback2, ViewTreeObserver.OnGlobalLayoutListener {
    public static final String META_DATA_LIB_NAME = "android.app.lib_name", META_DATA_FUNC_NAME = "android.app.func_name";

    public NativeActivity() {}
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Bundle meta = activityInfo().metaData;
        String lib = meta != null ? meta.getString(META_DATA_LIB_NAME, "main") : "main";
        snx.dalvik.Runtime.loadLibrary(lib);
        android.util.Log.w("NativeActivity", "ANativeActivity is not supported: lib" + lib + ".so is loaded, its entry point does not run");
    }
    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
    @Override public void surfaceRedrawNeeded(SurfaceHolder holder) {}
    @Override public void surfaceDestroyed(SurfaceHolder holder) {}
    @Override public void onGlobalLayout() {}
}
