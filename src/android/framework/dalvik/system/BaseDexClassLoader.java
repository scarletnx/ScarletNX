// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package dalvik.system;

import java.io.File;

/**
 * A class loader of DEX files. The app's own loader (snx.dalvik.AppClassLoader) is one, as on Android, and it
 * translates the classes of the APK. The runtime translates only the APK's DEX code, so a loader of another DEX path
 * finds its classes through its parent alone, and the others are not found.
 */
public class BaseDexClassLoader extends ClassLoader {
    private final String dexPath, librarySearchPath;

    public BaseDexClassLoader(String dexPath, File optimizedDirectory, String librarySearchPath, ClassLoader parent) {
        this(parent, null, dexPath, librarySearchPath);
    }
    public BaseDexClassLoader(String dexPath, File optimizedDirectory, String librarySearchPath, ClassLoader parent,
                              boolean isTrusted) {
        this(parent, null, dexPath, librarySearchPath);
    }
    /** For the app's loader, with the name of its JVM loader */
    protected BaseDexClassLoader(ClassLoader parent, String name, String dexPath, String librarySearchPath) {
        super(name, parent);
        this.dexPath = dexPath;
        this.librarySearchPath = librarySearchPath;
    }

    /** The path of lib{name}.so in the library search path, or null when no folder of it has the file */
    public String findLibrary(String name) {
        if (librarySearchPath == null) return null;
        for (String dir : librarySearchPath.split(File.pathSeparator)) {
            File file = new File(dir, System.mapLibraryName(name));
            if (!dir.isEmpty() && file.isFile()) return file.getPath();
        }
        return null;
    }

    public String getLdLibraryPath() { return librarySearchPath != null ? librarySearchPath : ""; }

    @Override public String toString() {
        return getClass().getName() + "[DexPathList[[zip file \"" + dexPath + "\"],nativeLibraryDirectories=["
            + getLdLibraryPath() + "]]]";
    }
}
