// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package dalvik.system;

/** The class loader of an app's APK, and of other DEX files on the file system (BaseDexClassLoader) */
public class PathClassLoader extends BaseDexClassLoader {
    public PathClassLoader(String dexPath, ClassLoader parent) { super(dexPath, (java.io.File) null, null, parent); }
    public PathClassLoader(String dexPath, String librarySearchPath, ClassLoader parent) {
        super(dexPath, (java.io.File) null, librarySearchPath, parent);
    }
    /** For the app's loader */
    protected PathClassLoader(ClassLoader parent, String name, String dexPath, String librarySearchPath) {
        super(parent, name, dexPath, librarySearchPath);
    }

    /** The two-argument form, public here as Android's ClassLoader.loadClass(String, boolean) is to its subclasses */
    @Override public Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        return super.loadClass(name, resolve);
    }
}
