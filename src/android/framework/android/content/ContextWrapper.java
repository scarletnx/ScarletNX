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

/** A context that hands each call to another: its base. */
public class ContextWrapper extends Context {
    Context mBase;

    public ContextWrapper(Context base) { mBase = base; }

    protected void attachBaseContext(Context base) {
        if (mBase != null) throw new IllegalStateException("Base context already set");
        mBase = base;
    }

    public Context getBaseContext() { return mBase; }

    @Override public AssetManager getAssets() { return mBase.getAssets(); }
    @Override public Resources getResources() { return mBase.getResources(); }
    @Override public PackageManager getPackageManager() { return mBase.getPackageManager(); }
    @Override public ContentResolver getContentResolver() { return mBase.getContentResolver(); }
    @Override public Looper getMainLooper() { return mBase.getMainLooper(); }
    @Override public Context getApplicationContext() { return mBase.getApplicationContext(); }
    @Override public void setTheme(int resid) { mBase.setTheme(resid); }
    @Override public Resources.Theme getTheme() { return mBase.getTheme(); }
    @Override public ClassLoader getClassLoader() { return mBase.getClassLoader(); }
    @Override public String getPackageName() { return mBase.getPackageName(); }
    @Override public ApplicationInfo getApplicationInfo() { return mBase.getApplicationInfo(); }
    @Override public String getPackageResourcePath() { return mBase.getPackageResourcePath(); }
    @Override public String getPackageCodePath() { return mBase.getPackageCodePath(); }
    @Override public SharedPreferences getSharedPreferences(String name, int mode) { return mBase.getSharedPreferences(name, mode); }
    @Override public boolean moveSharedPreferencesFrom(Context sourceContext, String name) { return mBase.moveSharedPreferencesFrom(sourceContext, name); }
    @Override public boolean deleteSharedPreferences(String name) { return mBase.deleteSharedPreferences(name); }
    @Override public FileInputStream openFileInput(String name) throws FileNotFoundException { return mBase.openFileInput(name); }
    @Override public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException { return mBase.openFileOutput(name, mode); }
    @Override public boolean deleteFile(String name) { return mBase.deleteFile(name); }
    @Override public File getFileStreamPath(String name) { return mBase.getFileStreamPath(name); }
    @Override public File getDataDir() { return mBase.getDataDir(); }
    @Override public File getFilesDir() { return mBase.getFilesDir(); }
    @Override public File getNoBackupFilesDir() { return mBase.getNoBackupFilesDir(); }
    @Override public File getExternalFilesDir(String type) { return mBase.getExternalFilesDir(type); }
    @Override public File[] getExternalFilesDirs(String type) { return mBase.getExternalFilesDirs(type); }
    @Override public File getObbDir() { return mBase.getObbDir(); }
    @Override public File[] getObbDirs() { return mBase.getObbDirs(); }
    @Override public File getCacheDir() { return mBase.getCacheDir(); }
    @Override public File getCodeCacheDir() { return mBase.getCodeCacheDir(); }
    @Override public File getExternalCacheDir() { return mBase.getExternalCacheDir(); }
    @Override public File[] getExternalCacheDirs() { return mBase.getExternalCacheDirs(); }
    @Override public File[] getExternalMediaDirs() { return mBase.getExternalMediaDirs(); }
    @Override public String[] fileList() { return mBase.fileList(); }
    @Override public File getDir(String name, int mode) { return mBase.getDir(name, mode); }
    @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory) { return mBase.openOrCreateDatabase(name, mode, factory); }
    @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory, DatabaseErrorHandler errorHandler) { return mBase.openOrCreateDatabase(name, mode, factory, errorHandler); }
    @Override public boolean moveDatabaseFrom(Context sourceContext, String name) { return mBase.moveDatabaseFrom(sourceContext, name); }
    @Override public boolean deleteDatabase(String name) { return mBase.deleteDatabase(name); }
    @Override public File getDatabasePath(String name) { return mBase.getDatabasePath(name); }
    @Override public String[] databaseList() { return mBase.databaseList(); }
    @Override public void startActivity(Intent intent) { mBase.startActivity(intent); }
    @Override public void startActivity(Intent intent, Bundle options) { mBase.startActivity(intent, options); }
    @Override public void startActivities(Intent[] intents) { mBase.startActivities(intents); }
    @Override public void startActivities(Intent[] intents, Bundle options) { mBase.startActivities(intents, options); }
    @Override public void startIntentSender(IntentSender intent, Intent fillInIntent, int flagsMask, int flagsValues, int extraFlags) throws IntentSender.SendIntentException { mBase.startIntentSender(intent, fillInIntent, flagsMask, flagsValues, extraFlags); }
    @Override public void sendBroadcast(Intent intent) { mBase.sendBroadcast(intent); }
    @Override public void sendBroadcast(Intent intent, String receiverPermission) { mBase.sendBroadcast(intent, receiverPermission); }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission) { mBase.sendOrderedBroadcast(intent, receiverPermission); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) { return mBase.registerReceiver(receiver, filter); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) { return mBase.registerReceiver(receiver, filter, flags); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler) { return mBase.registerReceiver(receiver, filter, broadcastPermission, scheduler); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler, int flags) { return mBase.registerReceiver(receiver, filter, broadcastPermission, scheduler, flags); }
    @Override public void unregisterReceiver(BroadcastReceiver receiver) { mBase.unregisterReceiver(receiver); }
    @Override public ComponentName startService(Intent service) { return mBase.startService(service); }
    @Override public ComponentName startForegroundService(Intent service) { return mBase.startForegroundService(service); }
    @Override public boolean stopService(Intent service) { return mBase.stopService(service); }
    @Override public boolean bindService(Intent service, ServiceConnection conn, int flags) { return mBase.bindService(service, conn, flags); }
    @Override public void unbindService(ServiceConnection conn) { mBase.unbindService(conn); }
    @Override public Object getSystemService(String name) { return mBase.getSystemService(name); }
    @Override public String getSystemServiceName(Class<?> serviceClass) { return mBase.getSystemServiceName(serviceClass); }
    @Override public int checkPermission(String permission, int pid, int uid) { return mBase.checkPermission(permission, pid, uid); }
    @Override public int checkCallingOrSelfPermission(String permission) { return mBase.checkCallingOrSelfPermission(permission); }
    @Override public int checkSelfPermission(String permission) { return mBase.checkSelfPermission(permission); }
    @Override public int checkCallingPermission(String permission) { return mBase.checkCallingPermission(permission); }
    @Override public void enforceCallingOrSelfPermission(String permission, String message) { mBase.enforceCallingOrSelfPermission(permission, message); }
    @Override public int checkUriPermission(Uri uri, int pid, int uid, int modeFlags) { return mBase.checkUriPermission(uri, pid, uid, modeFlags); }
    @Override public void grantUriPermission(String toPackage, Uri uri, int modeFlags) { mBase.grantUriPermission(toPackage, uri, modeFlags); }
    @Override public void revokeUriPermission(Uri uri, int modeFlags) { mBase.revokeUriPermission(uri, modeFlags); }
    @Override public Context createPackageContext(String packageName, int flags) throws PackageManager.NameNotFoundException { return mBase.createPackageContext(packageName, flags); }
    @Override public Context createConfigurationContext(Configuration overrideConfiguration) { return mBase.createConfigurationContext(overrideConfiguration); }
    @Override public Context createDisplayContext(Display display) { return mBase.createDisplayContext(display); }
    @Override public Context createDeviceProtectedStorageContext() { return mBase.createDeviceProtectedStorageContext(); }
    @Override public boolean isDeviceProtectedStorage() { return mBase.isDeviceProtectedStorage(); }
    @Override public Executor getMainExecutor() { return mBase.getMainExecutor(); }
    @Override public String getOpPackageName() { return mBase.getOpPackageName(); }
    @Override public boolean isRestricted() { return mBase.isRestricted(); }
    @Override public Display getDisplay() { return mBase.getDisplay(); }
    @Override public void registerComponentCallbacks(ComponentCallbacks callback) { mBase.registerComponentCallbacks(callback); }
    @Override public void unregisterComponentCallbacks(ComponentCallbacks callback) { mBase.unregisterComponentCallbacks(callback); }
}
