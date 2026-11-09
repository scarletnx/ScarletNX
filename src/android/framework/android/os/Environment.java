// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.File;

/** The device's storage folders, in the app's folder on the host (snx.android.Main.dataRoot). */
public class Environment {
    public static final String MEDIA_MOUNTED = "mounted";
    public static final String MEDIA_REMOVED = "removed";
    public static final String MEDIA_UNKNOWN = "unknown";
    public static String DIRECTORY_MUSIC = "Music";
    public static String DIRECTORY_PICTURES = "Pictures";
    public static String DIRECTORY_MOVIES = "Movies";
    public static String DIRECTORY_DOWNLOADS = "Download";
    public static String DIRECTORY_DCIM = "DCIM";
    public static String DIRECTORY_DOCUMENTS = "Documents";

    static File root(String name) {
        File f = new File(snx.android.Main.dataRoot(), name);
        f.mkdirs();
        return f;
    }

    public static File getRootDirectory() { return root("system"); }
    public static File getDataDirectory() { return root("data"); }
    public static File getDownloadCacheDirectory() { return root("cache"); }
    public static File getExternalStorageDirectory() { return root("sdcard"); }
    public static File getExternalStoragePublicDirectory(String type) { return new File(getExternalStorageDirectory(), type); }
    public static String getExternalStorageState() { return MEDIA_MOUNTED; }
    public static String getExternalStorageState(File path) { return MEDIA_MOUNTED; }
    @Deprecated public static String getStorageState(File path) { return getExternalStorageState(path); }
    public static boolean isExternalStorageRemovable() { return false; }
    public static boolean isExternalStorageEmulated() { return true; }
    /** "All files access", which an app that asks for MANAGE_EXTERNAL_STORAGE has, as each permission it asks for here */
    public static boolean isExternalStorageManager() {
        return snx.android.app.Package.get().permissions.contains("android.permission.MANAGE_EXTERNAL_STORAGE");
    }
    public static boolean isExternalStorageManager(File path) { return isExternalStorageManager(); }
    public static boolean isExternalStorageLegacy() { return false; }
}
