// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

/**
 * The app's APKs, which the runtime opened: the base and its splits, read by the runtime's zip reader. A member is
 * found in the base first, then in each split, as an asset pack's assets are found through the AssetManager.
 */
public final class Apk {
    private Apk() {}

    /** The path of the base APK on the host. */
    public static native String path();

    /** The paths of the split APKs, and their names (the manifests' split attributes). */
    public static native String[] splitPaths();
    public static native String[] splitNames();

    /** The folder of the app's native libraries, for its ABI (ApplicationInfo.nativeLibraryDir). */
    public static native String libraryDir();

    /** A member's bytes ("res/drawable/icon.xml"), or null when the APK has none. */
    public static synchronized byte[] read(String name) {
        return nativeRead(name);
    }

    /** The names of every member of every APK, the base's first. */
    public static synchronized String[] list() {
        return nativeList();
    }

    /** A member's bytes from one APK, 0 the base and 1 on each split in splitPaths' order; null when it has none. */
    public static synchronized byte[] readFrom(int apk, String name) {
        return nativeReadFrom(apk, name);
    }

    /** Whether a member is stored, not compressed, as Android needs one to open it as a file descriptor */
    public static synchronized boolean stored(String name) {
        return nativeStored(name);
    }

    private static native byte[] nativeRead(String name);

    private static native byte[] nativeReadFrom(int apk, String name);

    private static native boolean nativeStored(String name);

    private static native String[] nativeList();
}
