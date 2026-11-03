// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.Context;
import java.util.Map;
import java.util.function.Function;

/**
 * The system services of the process: each one is a local object (Abstract in docs/android/INTERFACES.md), no
 * binder. A context makes its own the first time it asks (the window manager and the inflater depend on it). The
 * services of absent hardware (Bluetooth, NFC, telecom) are not here: Android gives null for them too.
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
        Map.entry(Context.JOB_SCHEDULER_SERVICE, new Entry(android.app.job.JobScheduler.class, c -> new android.app.job.JobScheduler.Local(c))),
        Map.entry(Context.AUDIO_SERVICE, new Entry(android.media.AudioManager.class, c -> new android.media.AudioManager())),
        Map.entry(Context.LOCATION_SERVICE, new Entry(android.location.LocationManager.class, c -> new android.location.LocationManager())),
        Map.entry(Context.BATTERY_SERVICE, new Entry(android.os.BatteryManager.class, c -> new android.os.BatteryManager())),
        Map.entry(Context.DISPLAY_SERVICE, new Entry(android.hardware.display.DisplayManager.class, c -> new android.hardware.display.DisplayManager())),
        Map.entry(Context.INPUT_SERVICE, new Entry(android.hardware.input.InputManager.class, c -> new android.hardware.input.InputManager())),
        Map.entry(Context.WIFI_SERVICE, new Entry(android.net.wifi.WifiManager.class, c -> new android.net.wifi.WifiManager())),
        Map.entry(Context.NSD_SERVICE, new Entry(android.net.nsd.NsdManager.class, c -> new android.net.nsd.NsdManager())),
        Map.entry(Context.MEDIA_SESSION_SERVICE, new Entry(android.media.session.MediaSessionManager.class, c -> new android.media.session.MediaSessionManager())),
        Map.entry(Context.STORAGE_SERVICE, new Entry(android.os.storage.StorageManager.class, c -> new android.os.storage.StorageManager())),
        Map.entry(Context.BIOMETRIC_SERVICE, new Entry(android.hardware.biometrics.BiometricManager.class, c -> new android.hardware.biometrics.BiometricManager())),
        Map.entry(Context.FINGERPRINT_SERVICE, new Entry(android.hardware.fingerprint.FingerprintManager.class, c -> new android.hardware.fingerprint.FingerprintManager())),
        Map.entry(Context.CAMERA_SERVICE, new Entry(android.hardware.camera2.CameraManager.class, c -> new android.hardware.camera2.CameraManager())),
        Map.entry(Context.USB_SERVICE, new Entry(android.hardware.usb.UsbManager.class, c -> new android.hardware.usb.UsbManager())),
        Map.entry(Context.CONSUMER_IR_SERVICE, new Entry(android.hardware.ConsumerIrManager.class, c -> new android.hardware.ConsumerIrManager())),
        Map.entry(Context.MEDIA_PROJECTION_SERVICE, new Entry(android.media.projection.MediaProjectionManager.class, c -> new android.media.projection.MediaProjectionManager())),
        Map.entry(Context.TV_INPUT_SERVICE, new Entry(android.media.tv.TvInputManager.class, c -> new android.media.tv.TvInputManager())),
        Map.entry(Context.RESTRICTIONS_SERVICE, new Entry(android.content.RestrictionsManager.class, c -> new android.content.RestrictionsManager())),
        Map.entry(Context.LAUNCHER_APPS_SERVICE, new Entry(android.content.pm.LauncherApps.class, c -> new android.content.pm.LauncherApps())),
        Map.entry(Context.DROPBOX_SERVICE, new Entry(android.os.DropBoxManager.class, c -> new android.os.DropBoxManager())),
        Map.entry(Context.DOWNLOAD_SERVICE, new Entry(android.app.DownloadManager.class, c -> new android.app.DownloadManager())),
        Map.entry(Context.LOCALE_SERVICE, new Entry(android.app.LocaleManager.class, c -> new android.app.LocaleManager())),
        Map.entry(Context.SEARCH_SERVICE, new Entry(android.app.SearchManager.class, c -> new android.app.SearchManager())),
        Map.entry(Context.DEVICE_POLICY_SERVICE, new Entry(android.app.admin.DevicePolicyManager.class, c -> new android.app.admin.DevicePolicyManager())),
        Map.entry(Context.USAGE_STATS_SERVICE, new Entry(android.app.usage.UsageStatsManager.class, c -> new android.app.usage.UsageStatsManager())),
        Map.entry(Context.CREDENTIAL_SERVICE, new Entry(android.credentials.CredentialManager.class, c -> new android.credentials.CredentialManager())),
        Map.entry(Context.PRINT_SERVICE, new Entry(android.print.PrintManager.class, c -> new android.print.PrintManager())),
        Map.entry(Context.VIBRATOR_SERVICE, new Entry(android.os.Vibrator.class, c -> new android.os.Vibrator() {})),
        Map.entry(Context.WALLPAPER_SERVICE, new Entry(android.app.WallpaperManager.class, android.app.WallpaperManager::getInstance)),
        Map.entry(Context.CAPTIONING_SERVICE, new Entry(android.view.accessibility.CaptioningManager.class, android.view.accessibility.CaptioningManager::new)),
        Map.entry(Context.TEXT_CLASSIFICATION_SERVICE, new Entry(android.view.textclassifier.TextClassificationManager.class, android.view.textclassifier.TextClassificationManager::new)),
        Map.entry(Context.TEXT_SERVICES_MANAGER_SERVICE, new Entry(android.view.textservice.TextServicesManager.class, android.view.textservice.TextServicesManager::new)));

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
