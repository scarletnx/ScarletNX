// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.HashMap;

/** System properties: the few apps read, with Android 16's values. */
public class SystemProperties {
    private static final HashMap<String, String> values = new HashMap<>();
    static {
        values.put("ro.build.version.sdk", "36");
        values.put("ro.build.version.release", "16");
        values.put("ro.product.cpu.abi", "arm64-v8a");
        values.put("ro.debuggable", "0");
    }

    public static String get(String key) { return get(key, ""); }
    public static String get(String key, String def) { synchronized (values) { return values.getOrDefault(key, def); } }
    public static int getInt(String key, int def) {
        try { return Integer.parseInt(get(key)); } catch (NumberFormatException e) { return def; }
    }
    public static long getLong(String key, long def) {
        try { return Long.parseLong(get(key)); } catch (NumberFormatException e) { return def; }
    }
    public static boolean getBoolean(String key, boolean def) {
        String v = get(key);
        if (v.equals("1") || v.equals("y") || v.equals("yes") || v.equals("on") || v.equals("true")) return true;
        if (v.equals("0") || v.equals("n") || v.equals("no") || v.equals("off") || v.equals("false")) return false;
        return def;
    }
    public static void set(String key, String val) { synchronized (values) { values.put(key, val); } }
}
