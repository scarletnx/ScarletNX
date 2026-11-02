// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.util.HashMap;

/**
 * The base context of the application and of each component, as Android's ContextImpl: the app's resources and
 * theme, its folders under the data root, its preferences and databases, and the in-process system services.
 */
public final class ContextImpl extends Context {
    private static final HashMap<String, SharedPreferencesImpl> preferences = new HashMap<>();

    private final Resources resources;
    private final boolean deviceProtected;
    private Context outer;
    private int themeResource;
    private Resources.Theme theme;
    private final HashMap<String, Object> services = new HashMap<>();

    public ContextImpl(Resources resources, boolean deviceProtected) {
        this.resources = resources;
        this.deviceProtected = deviceProtected;
    }

    /** The context this one is the base of (the Application, an Activity): system services see it. */
    public void setOuter(Context outer) { this.outer = outer; }
    Context outer() { return outer != null ? outer : this; }

    @Override public AssetManager getAssets() { return resources.getAssets(); }
    @Override public Resources getResources() { return resources; }
    @Override public PackageManager getPackageManager() { return PackageManagerImpl.get(); }
    @Override public ContentResolver getContentResolver() { return ContentResolverImpl.get(); }
    @Override public Looper getMainLooper() { return Looper.getMainLooper(); }
    @Override public Context getApplicationContext() { return App.application(); }
    @Override public void setTheme(int resid) {
        if (themeResource == resid && theme != null) return;
        themeResource = resid;
        theme = null;
    }
    @Override public Resources.Theme getTheme() {
        if (theme == null) {
            theme = resources.newTheme();
            if (themeResource != 0) theme.applyStyle(themeResource, true);
        }
        return theme;
    }
    @Override public ClassLoader getClassLoader() { return App.classLoader(); }
    @Override public String getPackageName() { return Package.get().name(); }
    @Override public ApplicationInfo getApplicationInfo() { return Package.get().application; }
    @Override public String getPackageResourcePath() { return snx.android.Apk.path(); }
    @Override public String getPackageCodePath() { return snx.android.Apk.path(); }

    // ---- files ----
    private static File dir(File d) { d.mkdirs(); return d; }
    private static void checkName(String name) {
        if (name.indexOf(File.separatorChar) >= 0 || name.indexOf('/') >= 0)
            throw new IllegalArgumentException("File " + name + " contains a path separator");
    }
    @Override public File getDataDir() { return dir(deviceProtected ? new File(snx.android.Main.dataRoot(), "de") : snx.android.Main.dataRoot()); }
    @Override public File getFilesDir() { return dir(new File(getDataDir(), "files")); }
    @Override public File getNoBackupFilesDir() { return dir(new File(getDataDir(), "no_backup")); }
    @Override public File getCacheDir() { return dir(new File(getDataDir(), "cache")); }
    @Override public File getCodeCacheDir() { return dir(new File(getDataDir(), "code_cache")); }
    private File external() { return new File(snx.android.Main.dataRoot(), "external"); }
    @Override public File getExternalFilesDir(String type) { return dir(type != null ? new File(new File(external(), "files"), type) : new File(external(), "files")); }
    @Override public File[] getExternalFilesDirs(String type) { return new File[] {getExternalFilesDir(type)}; }
    @Override public File getObbDir() { return dir(new File(external(), "obb")); }
    @Override public File[] getObbDirs() { return new File[] {getObbDir()}; }
    @Override public File getExternalCacheDir() { return dir(new File(external(), "cache")); }
    @Override public File[] getExternalCacheDirs() { return new File[] {getExternalCacheDir()}; }
    @Override public File[] getExternalMediaDirs() { return new File[] {dir(new File(external(), "media"))}; }
    @Override public File getFileStreamPath(String name) { checkName(name); return new File(getFilesDir(), name); }
    @Override public FileInputStream openFileInput(String name) throws FileNotFoundException { return new FileInputStream(getFileStreamPath(name)); }
    @Override public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
        return new FileOutputStream(getFileStreamPath(name), (mode & MODE_APPEND) != 0);
    }
    @Override public boolean deleteFile(String name) { return getFileStreamPath(name).delete(); }
    @Override public String[] fileList() { String[] l = getFilesDir().list(); return l != null ? l : new String[0]; }
    @Override public File getDir(String name, int mode) { checkName(name); return dir(new File(getDataDir(), "app_" + name)); }

    // ---- preferences ----
    private File preferencesFile(String name) { checkName(name); return new File(dir(new File(getDataDir(), "shared_prefs")), name + ".xml"); }
    @Override public SharedPreferences getSharedPreferences(String name, int mode) {
        File f = preferencesFile(name == null ? "null" : name);
        synchronized (preferences) { return preferences.computeIfAbsent(f.getPath(), k -> new SharedPreferencesImpl(f)); }
    }
    @Override public boolean moveSharedPreferencesFrom(Context sourceContext, String name) {
        File from = ((ContextImpl) base(sourceContext)).preferencesFile(name), to = preferencesFile(name);
        synchronized (preferences) { preferences.remove(from.getPath()); preferences.remove(to.getPath()); }
        return !from.exists() || from.renameTo(to);
    }
    @Override public boolean deleteSharedPreferences(String name) {
        File f = preferencesFile(name);
        synchronized (preferences) { preferences.remove(f.getPath()); }
        return !f.exists() || f.delete();
    }
    private static Context base(Context c) {
        while (c instanceof android.content.ContextWrapper w) c = w.getBaseContext();
        return c;
    }

    // ---- databases ----
    @Override public File getDatabasePath(String name) {
        if (name.charAt(0) == File.separatorChar) return new File(name);
        checkName(name);
        return new File(dir(new File(getDataDir(), "databases")), name);
    }
    @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory) { return openOrCreateDatabase(name, mode, factory, null); }
    @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory, DatabaseErrorHandler errorHandler) {
        int flags = SQLiteDatabase.CREATE_IF_NECESSARY;
        if ((mode & MODE_ENABLE_WRITE_AHEAD_LOGGING) != 0) flags |= SQLiteDatabase.ENABLE_WRITE_AHEAD_LOGGING;
        return SQLiteDatabase.openDatabase(getDatabasePath(name).getPath(), factory, flags, errorHandler);
    }
    @Override public boolean moveDatabaseFrom(Context sourceContext, String name) {
        File from = sourceContext.getDatabasePath(name);
        return !from.exists() || from.renameTo(getDatabasePath(name));
    }
    @Override public boolean deleteDatabase(String name) { return SQLiteDatabase.deleteDatabase(getDatabasePath(name)); }
    @Override public String[] databaseList() { String[] l = new File(getDataDir(), "databases").list(); return l != null ? l : new String[0]; }

    // ---- components ----
    @Override public void startActivity(Intent intent) { startActivity(intent, null); }
    @Override public void startActivity(Intent intent, Bundle options) { Activities.start(null, intent, -1, options); }
    @Override public void startActivities(Intent[] intents) { for (Intent i : intents) startActivity(i); }
    @Override public void startActivities(Intent[] intents, Bundle options) { for (Intent i : intents) startActivity(i, options); }
    @Override public void startIntentSender(IntentSender intent, Intent fillInIntent, int flagsMask, int flagsValues, int extraFlags) throws IntentSender.SendIntentException {
        throw new IntentSender.SendIntentException("snx: no intent senders");
    }
    @Override public void sendBroadcast(Intent intent) { Broadcasts.send(intent); }
    @Override public void sendBroadcast(Intent intent, String receiverPermission) { Broadcasts.send(intent); }
    @Override public void sendOrderedBroadcast(Intent intent, String receiverPermission) { Broadcasts.send(intent); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) { return registerReceiver(receiver, filter, null, null, 0); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) { return registerReceiver(receiver, filter, null, null, flags); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler) {
        return registerReceiver(receiver, filter, broadcastPermission, scheduler, 0);
    }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission, Handler scheduler, int flags) {
        return Broadcasts.register(outer(), receiver, filter, scheduler);
    }
    @Override public void unregisterReceiver(BroadcastReceiver receiver) { Broadcasts.unregister(receiver); }
    @Override public ComponentName startService(Intent service) { return Services.start(service); }
    @Override public ComponentName startForegroundService(Intent service) { return Services.start(service); }
    @Override public boolean stopService(Intent service) { return Services.stop(service); }
    @Override public boolean bindService(Intent service, ServiceConnection conn, int flags) { return Services.bind(service, conn, flags); }
    @Override public void unbindService(ServiceConnection conn) { Services.unbind(conn); }

    // ---- system services ----
    @Override public Object getSystemService(String name) {
        if (name == null) return null;
        synchronized (services) {
            Object s = services.get(name);
            if (s == null && (s = SystemServices.make(outer(), name)) != null) services.put(name, s);
            return s;
        }
    }
    @Override public String getSystemServiceName(Class<?> serviceClass) { return SystemServices.name(serviceClass); }

    // ---- permissions: the app holds what it asks for ----
    @Override public int checkPermission(String permission, int pid, int uid) { return checkSelfPermission(permission); }
    @Override public int checkCallingOrSelfPermission(String permission) { return checkSelfPermission(permission); }
    @Override public int checkSelfPermission(String permission) {
        return Package.get().permissions.contains(permission) ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }
    @Override public int checkCallingPermission(String permission) { return checkSelfPermission(permission); }
    @Override public void enforceCallingOrSelfPermission(String permission, String message) {
        if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) throw new SecurityException(message);
    }
    @Override public int checkUriPermission(Uri uri, int pid, int uid, int modeFlags) { return PackageManager.PERMISSION_GRANTED; }
    @Override public void grantUriPermission(String toPackage, Uri uri, int modeFlags) {}
    @Override public void revokeUriPermission(Uri uri, int modeFlags) {}

    // ---- other contexts ----
    @Override public Context createPackageContext(String packageName, int flags) throws PackageManager.NameNotFoundException {
        if (!packageName.equals(getPackageName())) throw new PackageManager.NameNotFoundException(packageName);
        return new ContextImpl(resources, deviceProtected);
    }
    @Override public Context createConfigurationContext(Configuration overrideConfiguration) {
        Configuration c = new Configuration(resources.getConfiguration());
        c.updateFrom(overrideConfiguration);
        ContextImpl context = new ContextImpl(new Resources(resources.getAssets(), resources.getDisplayMetrics(), c), deviceProtected);
        context.themeResource = themeResource;
        return context;
    }
    @Override public Context createDisplayContext(Display display) { return new ContextImpl(resources, deviceProtected); }
    @Override public Context createDeviceProtectedStorageContext() { return new ContextImpl(resources, true); }
    @Override public boolean isDeviceProtectedStorage() { return deviceProtected; }
}
