// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.nativebridge;

/**
 * The app's native libraries: AArch64 code for bionic, recompiled and run in this process (docs/android/NATIVE.md).
 * Each library loads once, as Android's linker loads it.
 */
public final class NativeBridge {
    private static final java.util.HashSet<String> loaded = new java.util.HashSet<>();

    private NativeBridge() {}

    public static synchronized void load(String path) {
        if (loaded.contains(path)) return;
        String error = nativeLoad(path);
        if (error != null) throw new UnsatisfiedLinkError(error);
        loaded.add(path);
    }

    /** null when the library loaded (and its JNI_OnLoad ran), else dlerror's text. */
    private static String nativeLoad(String path) {
        return "dlopen failed: the native bridge cannot load " + new java.io.File(path).getName() + " yet";
    }
}
