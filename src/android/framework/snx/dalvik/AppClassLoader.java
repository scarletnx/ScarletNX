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

    /*
     * The app's Java resources: the APK's own files (and its splits'), as Android's PathClassLoader gives them, each a
     * jar: URL of its entry. libGDX reads its default font so, by Class.getResourceAsStream
     */
    private static java.util.List<java.util.zip.ZipFile> apks;
    private static synchronized java.util.List<java.util.zip.ZipFile> apks() {
        if (apks == null) {
            apks = new java.util.ArrayList<>();
            java.util.List<String> paths = new java.util.ArrayList<>();
            paths.add(snx.android.Apk.path());
            String[] splits = snx.android.Apk.splitPaths();
            if (splits != null) java.util.Collections.addAll(paths, splits);
            for (String path : paths) {
                try {
                    if (path != null) apks.add(new java.util.zip.ZipFile(path));
                } catch (java.io.IOException e) {
                    android.util.Log.w("snx", "the APK " + path + " is not readable for its resources: " + e);
                }
            }
        }
        return apks;
    }
    private static java.net.URL url(java.util.zip.ZipFile apk, String name) {
        try {
            return new java.net.URI("jar:" + new java.io.File(apk.getName()).toURI() + "!/" + name).toURL();
        } catch (java.net.URISyntaxException | java.net.MalformedURLException e) {
            return null;
        }
    }
    @Override
    protected java.net.URL findResource(String name) {
        for (java.util.zip.ZipFile apk : apks()) {
            java.util.zip.ZipEntry e = apk.getEntry(name);
            if (e != null && !e.isDirectory()) return url(apk, name);
        }
        return null;
    }
    @Override
    protected java.util.Enumeration<java.net.URL> findResources(String name) {
        java.util.List<java.net.URL> found = new java.util.ArrayList<>();
        for (java.util.zip.ZipFile apk : apks()) {
            java.util.zip.ZipEntry e = apk.getEntry(name);
            java.net.URL u = e != null && !e.isDirectory() ? url(apk, name) : null;
            if (u != null) found.add(u);
        }
        return java.util.Collections.enumeration(found);
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
