// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.dalvik;

/**
 * The app's classes: each translated from the APK's DEX code by the runtime (src/android/dex/translate.c) when it
 * is first loaded. The framework and the JDK come first, through the parent. It is a PathClassLoader, as an Android
 * app's loader is: apps cast it to BaseDexClassLoader for findLibrary.
 */
public final class AppClassLoader extends dalvik.system.PathClassLoader {
    static {
        registerAsParallelCapable();
    }

    public AppClassLoader(ClassLoader parent) {
        super(parent, "app", null, null);
    }

    /** lib{name}.so of the app's native library folder, or null */
    @Override
    public String findLibrary(String name) {
        java.io.File file = new java.io.File(snx.android.Apk.libraryDir(), System.mapLibraryName(name));
        return file.isFile() ? file.getPath() : null;
    }

    @Override
    public String getLdLibraryPath() { return snx.android.Apk.libraryDir(); }

    @Override
    public String toString() {
        return "dalvik.system.PathClassLoader[DexPathList[[zip file \"" + snx.android.Apk.path()
            + "\"],nativeLibraryDirectories=[" + snx.android.Apk.libraryDir() + "]]]";
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        byte[] bytes = translate(name);
        if (bytes == null) throw new ClassNotFoundException(name);
        return defineClass(name, bytes, 0, bytes.length);
    }

    /** The class file of a class of the APK ("a.b.C"), or null when the APK has no such class. */
    private static native byte[] translate(String name);
}
