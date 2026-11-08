// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.os.ParcelFileDescriptor;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.util.TreeSet;
import snx.android.Apk;

/**
 * The resource tables and assets of the app: the HLE of libandroidfw's AssetManager2 (src/android/res/arsc.c). Each
 * native call is made holding the manager's lock, as Android's AssetManager does.
 */
public final class AssetManager implements AutoCloseable {
    public static final int ACCESS_UNKNOWN = 0, ACCESS_RANDOM = 1, ACCESS_STREAMING = 2, ACCESS_BUFFER = 3;
    private long handle;

    public AssetManager() {
        handle = nativeCreate();
    }

    /** The app's APK: its resources.arsc, its cookie 1. */
    public static AssetManager forApk() {
        AssetManager a = new AssetManager();
        byte[] table = Apk.read("resources.arsc");
        if (table != null) synchronized (a) { nativeAddTable(a.handle, table); }
        return a;
    }

    synchronized long handle() {
        return handle;
    }

    public synchronized void setConfiguration(Configuration c, DisplayMetrics m) {
        java.util.Locale l = c.getLocales().get(0);
        nativeSetConfiguration(handle, l != null ? l.getLanguage() : null, l != null ? l.getCountry() : null,
            c.densityDpi != 0 ? c.densityDpi : m.densityDpi, android.os.Build.VERSION.SDK_INT, c.orientation,
            c.uiMode, c.screenWidthDp, c.screenHeightDp, c.smallestScreenWidthDp, c.screenLayout & 0xff,
            c.touchscreen, c.keyboard, c.navigation);
    }

    synchronized boolean getResourceValue(int resid, TypedValue out, boolean resolve) {
        if (nativeGetResourceValue(handle, resid, out, resolve) >= 0) return true;
        for (int i = 0; i < FRAMEWORK_STRING_IDS.length; ++i)
            if (FRAMEWORK_STRING_IDS[i] == resid) {
                out.type = TypedValue.TYPE_STRING;
                out.data = 0;
                out.resourceId = resid;
                out.assetCookie = 0;
                out.string = FRAMEWORK_STRINGS[i];
                return true;
            }
        return false;
    }

    synchronized String getPooledString(int cookie, int index) {
        return nativeGetPooledString(handle, cookie, index);
    }

    /** A bag's entries: 8 ints each, the key then TypedArray's 7. Null for a resource that is not a bag. */
    synchronized int[] getBag(int resid) {
        return nativeGetBag(handle, resid);
    }

    /*
     * The framework's own resources, which no table here holds: the values that the base Theme of Android's framework
     * gives the attributes that an app's theme leaves to it, and the names of the framework styles among them. Each
     * style has an ID of type 0xff, which no public framework resource uses.
     */
    private static final int[] FRAMEWORK_THEME = {
        0x01010309, 0x01ff0001, // android:attr/alertDialogTheme: @android:style/Theme.Dialog.Alert
    };
    private static final int[] FRAMEWORK_NAMED = {0x01ff0001};
    private static final String[] FRAMEWORK_NAMES = {"android:style/Theme.Dialog.Alert"};
    /* android.R.string's public strings that dialogs use, in English */
    private static final int[] FRAMEWORK_STRING_IDS = {0x01040000, 0x01040001, 0x01040003, 0x01040009, 0x0104000a,
                                                       0x0104000b, 0x0104000d, 0x0104000e, 0x0104000f, 0x01040013};
    private static final String[] FRAMEWORK_STRINGS = {"Cancel", "Copy", "Cut", "No", "OK", "Paste", "Select all",
                                                       "Unknown", "<Untitled>", "Yes"};

    synchronized String getResourceName(int resid) {
        String name = nativeGetResourceName(handle, resid);
        for (int i = 0; name == null && i < FRAMEWORK_NAMED.length; ++i)
            if (FRAMEWORK_NAMED[i] == resid) name = FRAMEWORK_NAMES[i];
        String[] strings = {"cancel", "copy", "cut", "no", "ok", "paste", "selectAll", "unknownName", "untitled", "yes"};
        for (int i = 0; name == null && i < FRAMEWORK_STRING_IDS.length; ++i)
            if (FRAMEWORK_STRING_IDS[i] == resid) name = "android:string/" + strings[i];
        return name;
    }

    synchronized int getResourceIdentifier(String name, String type, String pkg) {
        return nativeGetResourceIdentifier(handle, name, type, pkg);
    }

    synchronized long createTheme() {
        return nativeThemeCreate(handle);
    }

    void destroyTheme(long theme) {
        synchronized (this) { nativeThemeDestroy(theme); }
    }

    synchronized void applyStyle(long theme, int resid, boolean force) {
        nativeThemeApplyStyle(theme, resid, force);
    }

    synchronized void copyTheme(long dst, long src) {
        nativeThemeCopy(dst, src);
    }

    synchronized void clearTheme(long theme) {
        nativeThemeClear(theme);
    }

    synchronized boolean getThemeValue(long theme, int attr, TypedValue out, boolean resolve) {
        if (nativeThemeGetAttributeValue(theme, attr, out, resolve) >= 0) return true;
        for (int i = 0; i < FRAMEWORK_THEME.length; i += 2)
            if (FRAMEWORK_THEME[i] == attr) {
                out.type = TypedValue.TYPE_REFERENCE;
                out.data = FRAMEWORK_THEME[i + 1];
                out.resourceId = FRAMEWORK_THEME[i + 1];
                out.assetCookie = 0;
                out.string = null;
                return true;
            }
        return false;
    }

    synchronized int[] getThemeStyles(long theme) {
        return nativeThemeGetStyles(theme);
    }

    synchronized void applyStyle(long theme, XmlBlock.Parser xml, int defStyleAttr, int defStyleRes, int[] attrs,
                                 int[] values, int[] indices) {
        nativeApplyStyle(theme, xml != null ? xml.state() : 0, defStyleAttr, defStyleRes, attrs, values, indices);
    }

    synchronized boolean resolveAttrs(long theme, int defStyleAttr, int defStyleRes, int[] inValues, int[] attrs,
                                      int[] values, int[] indices) {
        return nativeResolveAttrs(theme, defStyleAttr, defStyleRes, inValues, attrs, values, indices);
    }

    synchronized boolean retrieveAttributes(XmlBlock.Parser xml, int[] attrs, int[] values, int[] indices) {
        return nativeRetrieveAttributes(handle, xml != null ? xml.state() : 0, attrs, values, indices);
    }

    public InputStream open(String fileName) throws IOException {
        return open(fileName, ACCESS_STREAMING);
    }

    public InputStream open(String fileName, int accessMode) throws IOException {
        byte[] data = Apk.read("assets/" + fileName);
        if (data == null) throw new FileNotFoundException(fileName);
        return new ByteArrayInputStream(data);
    }

    public InputStream openNonAsset(String fileName) throws IOException {
        byte[] data = Apk.read(fileName);
        if (data == null) throw new FileNotFoundException(fileName);
        return new ByteArrayInputStream(data);
    }

    /** An asset as a file: copied out of the APK to the app's cache once. */
    public AssetFileDescriptor openFd(String fileName) throws IOException {
        return fdOf("assets/" + fileName, "apk-assets/" + fileName);
    }

    /** A file of the APK outside assets/, copied out the same way. */
    public AssetFileDescriptor openNonAssetFd(String fileName) throws IOException {
        return fdOf(fileName, "apk-files/" + fileName);
    }

    /** A compressed member is refused, as Android refuses it: only a stored one can be read through a descriptor */
    private static AssetFileDescriptor fdOf(String member, String copy) throws IOException {
        byte[] data = Apk.read(member);
        if (data == null) throw new FileNotFoundException(member);
        if (!Apk.stored(member))
            throw new FileNotFoundException("This file can not be opened as a file descriptor; it is probably compressed");
        File file = new File(snx.android.Main.dataRoot(), copy);
        if (!file.isFile() || file.length() != data.length) {
            file.getParentFile().mkdirs();
            java.nio.file.Files.write(file.toPath(), data);
        }
        return new AssetFileDescriptor(ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY), 0, data.length);
    }

    public String[] list(String path) throws IOException {
        String prefix = "assets/" + (path.isEmpty() ? "" : path + "/");
        TreeSet<String> names = new TreeSet<>();
        for (String n : Apk.list()) {
            if (!n.startsWith(prefix)) continue;
            String rest = n.substring(prefix.length());
            int slash = rest.indexOf('/');
            names.add(slash < 0 ? rest : rest.substring(0, slash));
        }
        return names.toArray(new String[0]);
    }

    public XmlResourceParser openXmlResourceParser(String fileName) throws IOException {
        return openXmlResourceParser(1, fileName);
    }

    public XmlResourceParser openXmlResourceParser(int cookie, String fileName) throws IOException {
        byte[] data = Apk.read(fileName);
        if (data == null) throw new FileNotFoundException(fileName);
        return new XmlBlock(data).newParser();
    }

    public String[] getLocales() {
        return new String[] {java.util.Locale.getDefault().toLanguageTag()};
    }

    @Override
    public synchronized void close() {
        if (handle != 0) nativeDestroy(handle);
        handle = 0;
    }

    private static native long nativeCreate();
    private static native void nativeDestroy(long h);
    private static native int nativeAddTable(long h, byte[] table);
    private static native void nativeSetConfiguration(long h, String language, String country, int density, int sdk,
        int orientation, int uiMode, int widthDp, int heightDp, int smallestDp, int screenLayout, int touchscreen,
        int keyboard, int navigation);
    private static native int nativeGetResourceValue(long h, int resid, TypedValue out, boolean resolve);
    private static native String nativeGetPooledString(long h, int cookie, int index);
    private static native int[] nativeGetBag(long h, int resid);
    private static native String nativeGetResourceName(long h, int resid);
    private static native int nativeGetResourceIdentifier(long h, String name, String type, String pkg);
    private static native long nativeThemeCreate(long h);
    private static native void nativeThemeDestroy(long theme);
    private static native void nativeThemeApplyStyle(long theme, int resid, boolean force);
    private static native void nativeThemeCopy(long dst, long src);
    private static native void nativeThemeClear(long theme);
    private static native int nativeThemeGetAttributeValue(long theme, int attr, TypedValue out, boolean resolve);
    private static native int[] nativeThemeGetStyles(long theme);
    private static native void nativeApplyStyle(long theme, long xml, int defStyleAttr, int defStyleRes, int[] attrs,
        int[] values, int[] indices);
    private static native boolean nativeResolveAttrs(long theme, int defStyleAttr, int defStyleRes, int[] inValues,
        int[] attrs, int[] values, int[] indices);
    private static native boolean nativeRetrieveAttributes(long h, long xml, int[] attrs, int[] values, int[] indices);
}
