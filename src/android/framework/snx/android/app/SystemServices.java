// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.Context;
import java.util.Map;
import java.util.function.Function;

/**
 * The system services of the process: each one is a local object (Abstract in docs/android/INTERFACES.md), no
 * binder. A context makes its own the first time it asks (the window manager and the inflater depend on it).
 */
final class SystemServices {
    private record Entry(Class<?> type, Function<Context, Object> make) {}

    private static final Map<String, Entry> services = Map.ofEntries(
        Map.entry(Context.WINDOW_SERVICE, new Entry(android.view.WindowManager.class, WindowManagerImpl::new)),
        Map.entry(Context.LAYOUT_INFLATER_SERVICE, new Entry(android.view.LayoutInflater.class, android.view.LayoutInflater::create)),
        Map.entry(Context.ACTIVITY_SERVICE, new Entry(android.app.ActivityManager.class, c -> new android.app.ActivityManager())),
        Map.entry(Context.ALARM_SERVICE, new Entry(android.app.AlarmManager.class, c -> new android.app.AlarmManager())),
        Map.entry(Context.NOTIFICATION_SERVICE, new Entry(android.app.NotificationManager.class, c -> new android.app.NotificationManager())),
        Map.entry(Context.APP_OPS_SERVICE, new Entry(android.app.AppOpsManager.class, c -> new android.app.AppOpsManager())),
        Map.entry(Context.KEYGUARD_SERVICE, new Entry(android.app.KeyguardManager.class, c -> new android.app.KeyguardManager())),
        Map.entry(Context.UI_MODE_SERVICE, new Entry(android.app.UiModeManager.class, c -> new android.app.UiModeManager())),
        Map.entry(Context.CONNECTIVITY_SERVICE, new Entry(android.net.ConnectivityManager.class, c -> new android.net.ConnectivityManager())),
        Map.entry(Context.CLIPBOARD_SERVICE, new Entry(android.content.ClipboardManager.class, c -> clipboard())),
        Map.entry(Context.INPUT_METHOD_SERVICE, new Entry(android.view.inputmethod.InputMethodManager.class, c -> new android.view.inputmethod.InputMethodManager())),
        Map.entry(Context.ACCESSIBILITY_SERVICE, new Entry(android.view.accessibility.AccessibilityManager.class, c -> new android.view.accessibility.AccessibilityManager())),
        Map.entry(Context.AUTOFILL_MANAGER_SERVICE, new Entry(android.view.autofill.AutofillManager.class, c -> new android.view.autofill.AutofillManager())),
        Map.entry(Context.CONTENT_CAPTURE_MANAGER_SERVICE, new Entry(android.view.contentcapture.ContentCaptureManager.class, c -> null)),
        Map.entry(Context.POWER_SERVICE, new Entry(android.os.PowerManager.class, c -> new android.os.PowerManager())),
        Map.entry(Context.SHORTCUT_SERVICE, new Entry(android.content.pm.ShortcutManager.class, c -> new android.content.pm.ShortcutManager())),
        Map.entry(Context.SENSOR_SERVICE, new Entry(android.hardware.SensorManager.class, c -> new android.hardware.SensorManager())),
        Map.entry(Context.TELEPHONY_SERVICE, new Entry(android.telephony.TelephonyManager.class, c -> new android.telephony.TelephonyManager())),
        Map.entry(Context.USER_SERVICE, new Entry(android.os.UserManager.class, c -> new android.os.UserManager())),
        Map.entry(Context.JOB_SCHEDULER_SERVICE, new Entry(android.app.job.JobScheduler.class, c -> new android.app.job.JobScheduler.Local(c))));

    private static android.content.ClipboardManager clipboardManager;

    private SystemServices() {}

    /** One clipboard for the process, as the system has one. */
    private static synchronized android.content.ClipboardManager clipboard() {
        if (clipboardManager == null) clipboardManager = new android.content.ClipboardManager();
        return clipboardManager;
    }

    /** A new service for the context, null for a service this device does not have (as Android gives). */
    static Object make(Context context, String name) {
        Entry e = services.get(name);
        return e != null ? e.make().apply(context) : null;
    }

    static String name(Class<?> type) {
        for (Map.Entry<String, Entry> e : services.entrySet()) if (e.getValue().type() == type) return e.getKey();
        return null;
    }
}
