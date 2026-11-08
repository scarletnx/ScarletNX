// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.dalvik;

/** What translated DEX code calls for the instructions that have no JVM bytecode. */
public final class Runtime {
    private Runtime() {}

    /**
     * System.loadLibrary and Runtime.loadLibrary, which the translator sends here: lib{name}.so of the app's native
     * library folder, loaded by the native bridge (docs/android/NATIVE.md), never by HotSpot's loader.
     */
    public static void loadLibrary(String name) {
        if (name == null) throw new NullPointerException("libName == null");
        java.io.File file = new java.io.File(snx.android.Apk.libraryDir(), "lib" + name + ".so");
        if (!file.isFile())
            throw new UnsatisfiedLinkError("dlopen failed: library \"lib" + name + ".so\" not found");
        load(file.getPath());
    }
    public static void load(String path) {
        if (path == null) throw new NullPointerException("filename == null");
        snx.android.nativebridge.NativeBridge.load(path);
    }
    public static void runtimeLoadLibrary(java.lang.Runtime runtime, String name) { loadLibrary(name); }

    private static final java.lang.ref.Cleaner cleaner = java.lang.ref.Cleaner.create();
    /**
     * ByteBuffer.allocateDirect, which the translator sends here: a buffer in the memory that the app's native code
     * reaches. A 32-bit library's is its 4 GiB window, so the buffer is there while one runs, and its memory is freed
     * when the buffer is unreachable; else the JDK's own buffer.
     */
    public static java.nio.ByteBuffer allocateDirect(int capacity) {
        if (capacity < 0) throw new IllegalArgumentException("capacity < 0: (" + capacity + " < 0)");
        long[] address = new long[1];
        java.nio.ByteBuffer b = snx.android.nativebridge.NativeBridge.allocateGuest(capacity, address);
        if (b == null) return java.nio.ByteBuffer.allocateDirect(capacity);
        long a = address[0];
        cleaner.register(b, () -> snx.android.nativebridge.NativeBridge.freeGuest(a));
        return b;
    }
    public static void runtimeLoad(java.lang.Runtime runtime, String path) { load(path); }

    /**
     * fill-array-data: the payload's bytes, little-endian elements of width bytes, from element offset on. The
     * translator puts each byte in one char of a string constant.
     */
    public static void fillArrayData(Object array, int offset, String data, int width) {
        if (array == null) throw new NullPointerException("fill-array-data on a null array");
        int count = data.length() / width;
        for (int i = 0; i < count; ++i) {
            long value = 0;
            for (int b = width - 1; b >= 0; --b) value = value << 8 | (data.charAt(i * width + b) & 0xff);
            int at = offset / width + i;
            if (array instanceof int[] a) a[at] = (int) value;
            else if (array instanceof long[] a) a[at] = value;
            else if (array instanceof float[] a) a[at] = Float.intBitsToFloat((int) value);
            else if (array instanceof double[] a) a[at] = Double.longBitsToDouble(value);
            else if (array instanceof short[] a) a[at] = (short) value;
            else if (array instanceof char[] a) a[at] = (char) value;
            else if (array instanceof byte[] a) a[at] = (byte) value;
            else if (array instanceof boolean[] a) a[at] = value != 0;
            else throw new IllegalArgumentException("fill-array-data on a " + array.getClass().getName());
        }
    }

    /**
     * Class.forName(String) that native code calls through JNI: the class by the app's loader. The one-argument form
     * loads with its caller's loader, which for a JNI call is the loader of the native method that runs. On Android
     * that is the app's own class; here every native method is NativeBridge's, the framework's (jni.c).
     */
    public static Class<?> forName(String name) throws ClassNotFoundException {
        return Class.forName(name, true, snx.android.app.App.classLoader());
    }

    /** The JNI signature of a method or a constructor, "(II)Ljava/lang/String;", for FromReflectedMethod (jni.c) */
    public static String signature(java.lang.reflect.Executable e) {
        StringBuilder b = new StringBuilder("(");
        for (Class<?> p : e.getParameterTypes()) b.append(p.descriptorString());
        b.append(')');
        b.append(e instanceof java.lang.reflect.Method m ? m.getReturnType().descriptorString() : "V");
        return b.toString();
    }
}
