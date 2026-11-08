// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.provider;

import android.content.ContentResolver;
import android.net.Uri;

/** The system settings: a table of Android's defaults, which the app may write for itself. */
public final class Settings {
    public static final String ACTION_SETTINGS = "android.settings.SETTINGS",
        ACTION_APPLICATION_DETAILS_SETTINGS = "android.settings.APPLICATION_DETAILS_SETTINGS",
        ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION = "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";

    public static class SettingNotFoundException extends android.util.AndroidException {
        public SettingNotFoundException(String msg) { super(msg); }
    }

    /** A table of settings. */
    static class Table {
        final String authority;
        final java.util.HashMap<String, String> values = new java.util.HashMap<>();
        Table(String authority) { this.authority = authority; }
        synchronized String get(String name) { return values.get(name); }
        synchronized void put(String name, String value) { values.put(name, value); }
        Uri uri(String name) { return Uri.parse("content://settings/" + authority + "/" + name); }
    }

    static final Table GLOBAL = new Table("global"), SECURE = new Table("secure"), SYSTEM = new Table("system");
    static {
        GLOBAL.put("animator_duration_scale", "1.0");
        GLOBAL.put("transition_animation_scale", "1.0");
        GLOBAL.put("window_animation_scale", "1.0");
        GLOBAL.put("auto_time", "1");
        GLOBAL.put("airplane_mode_on", "0");
        GLOBAL.put("development_settings_enabled", "0");
        GLOBAL.put("adb_enabled", "0");
        GLOBAL.put("device_provisioned", "1");
        SECURE.put("android_id", "5ca71e7a5ca71e7a");
        SECURE.put("accessibility_enabled", "0");
        SECURE.put("enabled_accessibility_services", "");
        SECURE.put("default_input_method", "");
        SYSTEM.put("font_scale", "1.0");
        SYSTEM.put("haptic_feedback_enabled", "1");
        SYSTEM.put("time_12_24", "24");
        SYSTEM.put("screen_brightness", "128");
    }

    static String string(Table t, String name) { return t.get(name); }
    static int integer(Table t, String name, int def) { try { String v = t.get(name); return v != null ? (int) Double.parseDouble(v) : def; } catch (NumberFormatException e) { return def; } }
    static long longValue(Table t, String name, long def) { try { String v = t.get(name); return v != null ? (long) Double.parseDouble(v) : def; } catch (NumberFormatException e) { return def; } }
    static float floatValue(Table t, String name, float def) { try { String v = t.get(name); return v != null ? Float.parseFloat(v) : def; } catch (NumberFormatException e) { return def; } }
    static String need(Table t, String name) throws SettingNotFoundException {
        String v = t.get(name);
        if (v == null) throw new SettingNotFoundException(name);
        return v;
    }

    public static boolean canDrawOverlays(android.content.Context context) { return false; }

    public static final class Global {
        public static final String ANIMATOR_DURATION_SCALE = "animator_duration_scale", TRANSITION_ANIMATION_SCALE = "transition_animation_scale",
            WINDOW_ANIMATION_SCALE = "window_animation_scale", AUTO_TIME = "auto_time", AIRPLANE_MODE_ON = "airplane_mode_on",
            DEVELOPMENT_SETTINGS_ENABLED = "development_settings_enabled", ADB_ENABLED = "adb_enabled", DEVICE_PROVISIONED = "device_provisioned",
            DEVICE_NAME = "device_name";
        public static final Uri CONTENT_URI = Uri.parse("content://settings/global");
        public static Uri getUriFor(String name) { return GLOBAL.uri(name); }
        public static String getString(ContentResolver resolver, String name) { return string(GLOBAL, name); }
        public static boolean putString(ContentResolver resolver, String name, String value) { GLOBAL.put(name, value); return true; }
        public static int getInt(ContentResolver cr, String name, int def) { return integer(GLOBAL, name, def); }
        public static int getInt(ContentResolver cr, String name) throws SettingNotFoundException { return (int) Double.parseDouble(need(GLOBAL, name)); }
        public static long getLong(ContentResolver cr, String name, long def) { return longValue(GLOBAL, name, def); }
        public static float getFloat(ContentResolver cr, String name, float def) { return floatValue(GLOBAL, name, def); }
        public static float getFloat(ContentResolver cr, String name) throws SettingNotFoundException { return Float.parseFloat(need(GLOBAL, name)); }
        public static boolean putInt(ContentResolver cr, String name, int value) { GLOBAL.put(name, Integer.toString(value)); return true; }
    }

    public static final class Secure {
        public static final String ANDROID_ID = "android_id", ACCESSIBILITY_ENABLED = "accessibility_enabled",
            ENABLED_ACCESSIBILITY_SERVICES = "enabled_accessibility_services", DEFAULT_INPUT_METHOD = "default_input_method",
            LOCATION_MODE = "location_mode";
        public static final Uri CONTENT_URI = Uri.parse("content://settings/secure");
        public static Uri getUriFor(String name) { return SECURE.uri(name); }
        public static String getString(ContentResolver resolver, String name) { return string(SECURE, name); }
        public static boolean putString(ContentResolver resolver, String name, String value) { SECURE.put(name, value); return true; }
        public static int getInt(ContentResolver cr, String name, int def) { return integer(SECURE, name, def); }
        public static int getInt(ContentResolver cr, String name) throws SettingNotFoundException { return (int) Double.parseDouble(need(SECURE, name)); }
        public static long getLong(ContentResolver cr, String name, long def) { return longValue(SECURE, name, def); }
        public static float getFloat(ContentResolver cr, String name, float def) { return floatValue(SECURE, name, def); }
    }

    public static final class System {
        public static final String FONT_SCALE = "font_scale", HAPTIC_FEEDBACK_ENABLED = "haptic_feedback_enabled", TIME_12_24 = "time_12_24",
            SCREEN_BRIGHTNESS = "screen_brightness", ACCELEROMETER_ROTATION = "accelerometer_rotation";
        public static final Uri CONTENT_URI = Uri.parse("content://settings/system");
        public static final String RINGTONE = "ringtone", NOTIFICATION_SOUND = "notification_sound", ALARM_ALERT = "alarm_alert";
        public static final Uri DEFAULT_RINGTONE_URI = Uri.parse("content://settings/system/ringtone"),
            DEFAULT_NOTIFICATION_URI = Uri.parse("content://settings/system/notification_sound"),
            DEFAULT_ALARM_ALERT_URI = Uri.parse("content://settings/system/alarm_alert");
        public static Uri getUriFor(String name) { return SYSTEM.uri(name); }
        public static String getString(ContentResolver resolver, String name) { return string(SYSTEM, name); }
        public static boolean putString(ContentResolver resolver, String name, String value) { SYSTEM.put(name, value); return true; }
        public static int getInt(ContentResolver cr, String name, int def) { return integer(SYSTEM, name, def); }
        public static int getInt(ContentResolver cr, String name) throws SettingNotFoundException { return (int) Double.parseDouble(need(SYSTEM, name)); }
        public static long getLong(ContentResolver cr, String name, long def) { return longValue(SYSTEM, name, def); }
        public static float getFloat(ContentResolver cr, String name, float def) { return floatValue(SYSTEM, name, def); }
        public static boolean canWrite(android.content.Context context) { return false; }
    }
}
