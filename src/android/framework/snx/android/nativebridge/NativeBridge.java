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
    private static String nativeLoad(String path) { return nativeLoad(path, snx.android.Apk.libraryDir()); }
    private static native String nativeLoad(String path, String libraryDir);

    /**
     * A native method of the app, which the translator makes call these: its key ("Lpkg/Class;.name(signature)"),
     * the object (or the class of a static method), its primitive parameters in order as their bits (a float's and a
     * double's raw bits), and its references. The result's bits, or the reference.
     */
    public static native long invokeJ(String key, Object self, long[] prims, Object[] refs);
    public static native Object invokeL(String key, Object self, long[] prims, Object[] refs);
    public static native void invokeV(String key, Object self, long[] prims, Object[] refs);
}
