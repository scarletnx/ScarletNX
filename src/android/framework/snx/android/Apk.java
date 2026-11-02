// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

/** The APK the runtime opened: its members, read by the runtime's zip reader. */
public final class Apk {
    private Apk() {}

    /** The path of the APK on the host. */
    public static native String path();

    /** A member's bytes ("res/drawable/icon.xml"), or null when the APK has none. */
    public static synchronized byte[] read(String name) {
        return nativeRead(name);
    }

    /** The names of every member, in the APK's order. */
    public static synchronized String[] list() {
        return nativeList();
    }

    private static native byte[] nativeRead(String name);

    private static native String[] nativeList();
}
