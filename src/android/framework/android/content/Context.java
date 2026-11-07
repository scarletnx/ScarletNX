// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.Display;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.util.concurrent.Executor;

/** The app's environment: its resources, files, components and the system services. */
public abstract class Context {
    public static final int MODE_PRIVATE = 0, MODE_WORLD_READABLE = 1, MODE_WORLD_WRITEABLE = 2, MODE_APPEND = 0x8000,
        MODE_MULTI_PROCESS = 4, MODE_ENABLE_WRITE_AHEAD_LOGGING = 8, MODE_NO_LOCALIZED_COLLATORS = 0x10;
    public static final int BIND_AUTO_CREATE = 1, BIND_DEBUG_UNBIND = 2, BIND_NOT_FOREGROUND = 4, BIND_ABOVE_CLIENT = 8,
        BIND_ALLOW_OOM_MANAGEMENT = 0x10, BIND_WAIVE_PRIORITY = 0x20, BIND_IMPORTANT = 0x40, BIND_ADJUST_WITH_ACTIVITY = 0x80,
        BIND_NOT_PERCEPTIBLE = 0x100, BIND_INCLUDE_CAPABILITIES = 0x1000;
    public static final int RECEIVER_VISIBLE_TO_INSTANT_APPS = 1, RECEIVER_EXPORTED = 2, RECEIVER_NOT_EXPORTED = 4;
    public static final int CONTEXT_INCLUDE_CODE = 1, CONTEXT_IGNORE_SECURITY = 2, CONTEXT_RESTRICTED = 4;
    public static final String ACTIVITY_SERVICE = "activity";
    public static final String ALARM_SERVICE = "alarm";
    public static final String APP_OPS_SERVICE = "appops";
    public static final String AUDIO_SERVICE = "audio";
    public static final String AUTOFILL_MANAGER_SERVICE = "autofill";
    public static final String ACCESSIBILITY_SERVICE = "accessibility";
    public static final String CLIPBOARD_SERVICE = "clipboard";
    public static final String CONNECTIVITY_SERVICE = "connectivity";
    public static final String CONTENT_CAPTURE_MANAGER_SERVICE = "content_capture";
    public static final String DISPLAY_SERVICE = "display";
    public static final String DOWNLOAD_SERVICE = "download";
    public static final String INPUT_METHOD_SERVICE = "input_method";
    public static final String INPUT_SERVICE = "input";
    public static final String JOB_SCHEDULER_SERVICE = "jobscheduler";
    public static final String KEYGUARD_SERVICE = "keyguard";
    public static final String LAYOUT_INFLATER_SERVICE = "layout_inflater";
    public static final String LOCATION_SERVICE = "location";
    public static final String NOTIFICATION_SERVICE = "notification";
    public static final String POWER_SERVICE = "power";
    public static final String SEARCH_SERVICE = "search";
    public static final String SENSOR_SERVICE = "sensor";
    public static final String STORAGE_SERVICE = "storage";
    public static final String TELEPHONY_SERVICE = "phone";
    public static final String TEXT_CLASSIFICATION_SERVICE = "textclassification";
    public static final String TEXT_SERVICES_MANAGER_SERVICE = "textservices";
    public static final String UI_MODE_SERVICE = "uimode";
    public static final String USER_SERVICE = "user";
    public static final String VIBRATOR_SERVICE = "vibrator";
    public static final String VIBRATOR_MANAGER_SERVICE = "vibrator_manager";
    public static final String WALLPAPER_SERVICE = "wallpaper";
    public static final String WIFI_SERVICE = "wifi";
    public static final String WINDOW_SERVICE = "window";
    public static final String BATTERY_SERVICE = "batterymanager";
    public static final String CAMERA_SERVICE = "camera";
    public static final String DEVICE_POLICY_SERVICE = "device_policy";
    public static final String LOCALE_SERVICE = "locale";
    public static final String TRANSLATION_MANAGER_SERVICE = "translation";
    public static final String STATUS_BAR_SERVICE = "statusbar";
    public static final String CREDENTIAL_SERVICE = "credential";
    public static final String PRINT_SERVICE = "print";
    public static final String SHORTCUT_SERVICE = "shortcut";
    public static final String USAGE_STATS_SERVICE = "usagestats";
    public static final String MEDIA_SESSION_SERVICE = "media_session";
    public static final String NETWORK_STATS_SERVICE = "netstats";
    public static final String PERFORMANCE_HINT_SERVICE = "performance_hint";
    public static final String GAME_SERVICE = "game";
    public static final String NSD_SERVICE = "servicediscovery";
    public static final String BIOMETRIC_SERVICE = "biometric";
    public static final String FINGERPRINT_SERVICE = "fingerprint";
    public static final String USB_SERVICE = "usb";
    public static final String CONSUMER_IR_SERVICE = "consumer_ir";
    public static final String MEDIA_PROJECTION_SERVICE = "media_projection";
    public static final String TV_INPUT_SERVICE = "tv_input";
    public static final String RESTRICTIONS_SERVICE = "restrictions";
    public static final String LAUNCHER_APPS_SERVICE = "launcherapps";
    public static final String DROPBOX_SERVICE = "dropbox";
    public static final String CAPTIONING_SERVICE = "captioning";

    public Context() {}

    public abstract AssetManager getAssets();
    public abstract Resources getResources();
    public abstract PackageManager getPackageManager();
    public abstract ContentResolver getContentResolver();
    public abstract Looper getMainLooper();
    public abstract Context getApplicationContext();
    public abstract void setTheme(int resid);
    public abstract Resources.Theme getTheme();
    public abstract ClassLoader getClassLoader();
    public abstract String getPackageName();
    public abstract ApplicationInfo getApplicationInfo();
    public abstract String getPackageResourcePath();
    public abstract String getPackageCodePath();
    public abstract SharedPreferences getSharedPreferences(String name, int mode);
    public abstract boolean moveSharedPreferencesFrom(Context sourceContext, String name);
    public abstract boolean deleteSharedPreferences(String name);
    public abstract FileInputStream openFileInput(String name) throws FileNotFoundException;
    public abstract FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException;
    public abstract boolean deleteFile(String name);
    public abstract File getFileStreamPath(String name);
    public abstract File getDataDir();
    public abstract File getFilesDir();
    public abstract File getNoBackupFilesDir();
    public abstract File getExternalFilesDir(String type);
    public abstract File[] getExternalFilesDirs(String type);
    public abstract File getObbDir();
    public abstract File[] getObbDirs();
    public abstract File getCacheDir();
    public abstract File getCodeCacheDir();
    public abstract File getExternalCacheDir();
    public abstract File[] getExternalCacheDirs();
    public abstract File[] getExternalMediaDirs();
    public abstract String[] fileList();
    public abstract File getDir(String name, int mode);
    public abstract SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory);
    public abstract SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory, DatabaseErrorHandler errorHandler);
    public abstract boolean moveDatabaseFrom(Context sourceContext, String name);
    public abstract boolean deleteDatabase(String name);
    public abstract File getDatabasePath(String name);
    public abstract String[] databaseList();
    public abstract void startActivity(Intent intent);
    public abstract void startActivity(Intent intent, Bundle options);
    public abstract void startActivities(Intent[] intents);
    public abstract void startActivities(Intent[] intents, Bundle options);
    public abstract void startIntentSender(IntentSender intent, Intent fillInIntent, int flagsMask, int flagsValues, int extraFlags) throws IntentSender.SendIntentException;
    public abstract void sendBroadcast(Intent intent);
    public abstract void sendBroadcast(Intent intent, String receiverPermission);
    public abstract void sendOrderedBroadcast(Intent intent, String receiverPermission);
    public abstract Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter);
    public abstract Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags);
    public abstract Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler);
    public abstract Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler, int flags);
    public abstract void unregisterReceiver(BroadcastReceiver receiver);
    public abstract ComponentName startService(Intent service);
    public abstract ComponentName startForegroundService(Intent service);
    public abstract boolean stopService(Intent service);
    public abstract boolean bindService(Intent service, ServiceConnection conn, int flags);
    public abstract void unbindService(ServiceConnection conn);
    public abstract Object getSystemService(String name);
    public abstract String getSystemServiceName(Class<?> serviceClass);
    public abstract int checkPermission(String permission, int pid, int uid);
    public abstract int checkCallingOrSelfPermission(String permission);
    public abstract int checkSelfPermission(String permission);
    public abstract int checkCallingPermission(String permission);
    public abstract void enforceCallingOrSelfPermission(String permission, String message);
    public abstract int checkUriPermission(Uri uri, int pid, int uid, int modeFlags);
    public abstract void grantUriPermission(String toPackage, Uri uri, int modeFlags);
    public abstract void revokeUriPermission(Uri uri, int modeFlags);
    public abstract Context createPackageContext(String packageName, int flags) throws PackageManager.NameNotFoundException;
    public abstract Context createConfigurationContext(Configuration overrideConfiguration);
    public abstract Context createDisplayContext(Display display);
    public abstract Context createDeviceProtectedStorageContext();
    public abstract boolean isDeviceProtectedStorage();

    public Executor getMainExecutor() { Handler h = new Handler(getMainLooper()); return h::post; }
    public String getOpPackageName() { return getPackageName(); }
    public String getAttributionTag() { return null; }
    public Context createAttributionContext(String attributionTag) { return this; }
    public Context createContext(Object contextParams) { return this; }
    public Context createWindowContext(int type, Bundle options) { return this; }
    public Context createWindowContext(Display display, int type, Bundle options) { return this; }
    /** Each split's code and resources are in the one context of the app. */
    public Context createContextForSplit(String splitName) throws PackageManager.NameNotFoundException { return this; }
    public int checkCallingOrSelfUriPermission(Uri uri, int modeFlags) {
        return checkUriPermission(uri, android.os.Process.myPid(), android.os.Process.myUid(), modeFlags);
    }
    /** The receivers of the process take the broadcast, then resultReceiver takes the result last. */
    public void sendOrderedBroadcast(Intent intent, String receiverPermission, BroadcastReceiver resultReceiver,
            Handler scheduler, int initialCode, String initialData, Bundle initialExtras) {
        sendOrderedBroadcast(intent, receiverPermission);
        if (resultReceiver == null) return;
        BroadcastReceiver.PendingResult result = new BroadcastReceiver.PendingResult();
        result.setResult(initialCode, initialData, initialExtras);
        Handler h = scheduler != null ? scheduler : new Handler(getMainLooper());
        h.post(() -> {
            resultReceiver.setPendingResult(result);
            resultReceiver.onReceive(this, intent);
        });
    }
    public boolean isRestricted() { return false; }
    public boolean isUiContext() { return false; }
    public Display getDisplay() { return Display.DEFAULT; }
    public int getDisplayId() { return 0; }
    public void registerComponentCallbacks(ComponentCallbacks callback) {
        Context app = getApplicationContext();
        if (app != null && app != this) app.registerComponentCallbacks(callback);
    }
    public void unregisterComponentCallbacks(ComponentCallbacks callback) {
        Context app = getApplicationContext();
        if (app != null && app != this) app.unregisterComponentCallbacks(callback);
    }
    public final CharSequence getText(int resId) { return getResources().getText(resId); }
    public final String getString(int resId) { return getResources().getString(resId); }
    public final String getString(int resId, Object... formatArgs) { return getResources().getString(resId, formatArgs); }
    public final int getColor(int id) { return getResources().getColor(id, getTheme()); }
    public final android.graphics.drawable.Drawable getDrawable(int id) { return getResources().getDrawable(id, getTheme()); }
    public final android.content.res.ColorStateList getColorStateList(int id) { return getResources().getColorStateList(id, getTheme()); }
    public final TypedArray obtainStyledAttributes(int[] attrs) { return getTheme().obtainStyledAttributes(attrs); }
    public final TypedArray obtainStyledAttributes(int resid, int[] attrs) { return getTheme().obtainStyledAttributes(resid, attrs); }
    public final TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs) { return getTheme().obtainStyledAttributes(set, attrs, 0, 0); }
    public final TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
        return getTheme().obtainStyledAttributes(set, attrs, defStyleAttr, defStyleRes);
    }
    @SuppressWarnings("unchecked")
    public final <T> T getSystemService(Class<T> serviceClass) {
        String name = getSystemServiceName(serviceClass);
        return name != null ? (T) getSystemService(name) : null;
    }
    public void sendBroadcastAsUser(Intent intent, android.os.UserHandle user) { sendBroadcast(intent); }
    public boolean bindService(Intent service, int flags, Executor executor, ServiceConnection conn) { return bindService(service, conn, flags); }
    public boolean bindIsolatedService(Intent service, int flags, String instanceName, Executor executor, ServiceConnection conn) { return bindService(service, conn, flags); }
    public void updateServiceGroup(ServiceConnection conn, int group, int importance) {}
}
