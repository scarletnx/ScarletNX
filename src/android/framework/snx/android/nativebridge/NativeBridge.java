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

    /** A zeroed direct buffer of capacity bytes in the 32-bit bridge's window, its guest address in address[0]; null
        when no 32-bit library runs */
    public static native java.nio.ByteBuffer allocateGuest(int capacity, long[] address);
    public static native void freeGuest(long address);

    /** NativeActivity's glue: its ANativeActivity made and the library's function of that name called with it (0 when
        the native bridge has no glue for the library), then the callbacks that the library set, by their index */
    public static native long activityCreate(Object activity, String function, String internalPath, String externalPath, int sdk);
    public static native void activityCall(long activity, int callback, long argument);
    /** an AInputQueue (0 when the native bridge has none), and the events sent to it */
    public static native long inputQueueNew();
    public static native void inputQueueMotion(long queue, int device, int source, int action, int flags, int meta, int buttons,
                                               int edges, long downNanos, long eventNanos, float xPrecision, float yPrecision,
                                               int[] ids, int[] tools, float[] axes);
    public static native void inputQueueKey(long queue, int device, int source, int action, int flags, int keyCode,
                                            int scanCode, int meta, int repeat, long downNanos, long eventNanos);
}
