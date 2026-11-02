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
}
