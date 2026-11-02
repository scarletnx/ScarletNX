// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.dalvik;

/**
 * The app's classes: each translated from the APK's DEX code by the runtime (src/android/dex/translate.c) when it
 * is first loaded. The framework and the JDK come first, through the parent.
 */
public final class AppClassLoader extends ClassLoader {
    static {
        registerAsParallelCapable();
    }

    public AppClassLoader(ClassLoader parent) {
        super("app", parent);
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
