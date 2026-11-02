// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.drawable.Drawable;
import java.util.List;

/** The installed packages: the app alone. */
public abstract class PackageManager {
    public static final int GET_ACTIVITIES = 0x1, GET_RECEIVERS = 0x2, GET_SERVICES = 0x4, GET_PROVIDERS = 0x8,
        GET_INSTRUMENTATION = 0x10, GET_INTENT_FILTERS = 0x20, GET_SIGNATURES = 0x40, GET_RESOLVED_FILTER = 0x40,
        GET_META_DATA = 0x80, GET_GIDS = 0x100, GET_DISABLED_COMPONENTS = 0x200, MATCH_DISABLED_COMPONENTS = 0x200,
        GET_SHARED_LIBRARY_FILES = 0x400, GET_URI_PERMISSION_PATTERNS = 0x800, GET_PERMISSIONS = 0x1000,
        GET_UNINSTALLED_PACKAGES = 0x2000, MATCH_UNINSTALLED_PACKAGES = 0x2000, GET_CONFIGURATIONS = 0x4000,
        MATCH_DISABLED_UNTIL_USED_COMPONENTS = 0x8000, MATCH_DEFAULT_ONLY = 0x10000, MATCH_ALL = 0x20000,
        MATCH_DIRECT_BOOT_UNAWARE = 0x40000, MATCH_DIRECT_BOOT_AWARE = 0x80000, MATCH_SYSTEM_ONLY = 0x100000,
        GET_SIGNING_CERTIFICATES = 0x8000000, MATCH_APEX = 0x40000000;
    public static final int COMPONENT_ENABLED_STATE_DEFAULT = 0, COMPONENT_ENABLED_STATE_ENABLED = 1,
        COMPONENT_ENABLED_STATE_DISABLED = 2, COMPONENT_ENABLED_STATE_DISABLED_USER = 3,
        COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED = 4;
    public static final int DONT_KILL_APP = 1, SYNCHRONOUS = 2;
    public static final int PERMISSION_GRANTED = 0, PERMISSION_DENIED = -1;
    public static final int SIGNATURE_MATCH = 0, SIGNATURE_NO_MATCH = -3, SIGNATURE_UNKNOWN_PACKAGE = -4;
    public static final String FEATURE_TOUCHSCREEN = "android.hardware.touchscreen", FEATURE_WIFI = "android.hardware.wifi",
        FEATURE_CAMERA = "android.hardware.camera", FEATURE_TELEPHONY = "android.hardware.telephony",
        FEATURE_LEANBACK = "android.software.leanback", FEATURE_WATCH = "android.hardware.type.watch",
        FEATURE_AUTOMOTIVE = "android.hardware.type.automotive", FEATURE_PC = "android.hardware.type.pc",
        FEATURE_FAKETOUCH = "android.hardware.faketouch", FEATURE_SCREEN_PORTRAIT = "android.hardware.screen.portrait",
        FEATURE_SCREEN_LANDSCAPE = "android.hardware.screen.landscape", FEATURE_VULKAN_HARDWARE_VERSION = "android.hardware.vulkan.version",
        FEATURE_APP_WIDGETS = "android.software.app_widgets", FEATURE_INPUT_METHODS = "android.software.input_methods",
        FEATURE_BACKUP = "android.software.backup", FEATURE_PRINTING = "android.software.print",
        FEATURE_EMBEDDED = "android.hardware.type.embedded";

    public static class NameNotFoundException extends android.util.AndroidException {
        public NameNotFoundException() {}
        public NameNotFoundException(String name) { super(name); }
    }

    public static final class PackageInfoFlags {
        private final long value;
        private PackageInfoFlags(long value) { this.value = value; }
        public static PackageInfoFlags of(long value) { return new PackageInfoFlags(value); }
        public long getValue() { return value; }
    }

    public static final class ApplicationInfoFlags {
        private final long value;
        private ApplicationInfoFlags(long value) { this.value = value; }
        public static ApplicationInfoFlags of(long value) { return new ApplicationInfoFlags(value); }
        public long getValue() { return value; }
    }

    public static final class ComponentInfoFlags {
        private final long value;
        private ComponentInfoFlags(long value) { this.value = value; }
        public static ComponentInfoFlags of(long value) { return new ComponentInfoFlags(value); }
        public long getValue() { return value; }
    }

    public static final class ResolveInfoFlags {
        private final long value;
        private ResolveInfoFlags(long value) { this.value = value; }
        public static ResolveInfoFlags of(long value) { return new ResolveInfoFlags(value); }
        public long getValue() { return value; }
    }

    public abstract PackageInfo getPackageInfo(String packageName, int flags) throws NameNotFoundException;
    public PackageInfo getPackageInfo(String packageName, PackageInfoFlags flags) throws NameNotFoundException { return getPackageInfo(packageName, (int) flags.getValue()); }
    public abstract ApplicationInfo getApplicationInfo(String packageName, int flags) throws NameNotFoundException;
    public ApplicationInfo getApplicationInfo(String packageName, ApplicationInfoFlags flags) throws NameNotFoundException { return getApplicationInfo(packageName, (int) flags.getValue()); }
    public abstract ActivityInfo getActivityInfo(ComponentName component, int flags) throws NameNotFoundException;
    public ActivityInfo getActivityInfo(ComponentName component, ComponentInfoFlags flags) throws NameNotFoundException { return getActivityInfo(component, (int) flags.getValue()); }
    public abstract ActivityInfo getReceiverInfo(ComponentName component, int flags) throws NameNotFoundException;
    public abstract ServiceInfo getServiceInfo(ComponentName component, int flags) throws NameNotFoundException;
    public abstract ProviderInfo getProviderInfo(ComponentName component, int flags) throws NameNotFoundException;
    public ProviderInfo getProviderInfo(ComponentName component, ComponentInfoFlags flags) throws NameNotFoundException { return getProviderInfo(component, (int) flags.getValue()); }
    public abstract ProviderInfo resolveContentProvider(String authority, int flags);
    public abstract List<PackageInfo> getInstalledPackages(int flags);
    public abstract List<ApplicationInfo> getInstalledApplications(int flags);
    public abstract List<ResolveInfo> queryIntentActivities(Intent intent, int flags);
    public List<ResolveInfo> queryIntentActivities(Intent intent, ResolveInfoFlags flags) { return queryIntentActivities(intent, (int) flags.getValue()); }
    public abstract List<ResolveInfo> queryIntentServices(Intent intent, int flags);
    public abstract List<ResolveInfo> queryBroadcastReceivers(Intent intent, int flags);
    public abstract List<ResolveInfo> queryIntentContentProviders(Intent intent, int flags);
    public abstract ResolveInfo resolveActivity(Intent intent, int flags);
    public ResolveInfo resolveActivity(Intent intent, ResolveInfoFlags flags) { return resolveActivity(intent, (int) flags.getValue()); }
    public abstract ResolveInfo resolveService(Intent intent, int flags);
    public abstract Intent getLaunchIntentForPackage(String packageName);
    public abstract int checkPermission(String permName, String packageName);
    public abstract boolean hasSystemFeature(String featureName);
    public boolean hasSystemFeature(String featureName, int version) { return hasSystemFeature(featureName); }
    public FeatureInfo[] getSystemAvailableFeatures() { return new FeatureInfo[0]; }
    public abstract void setComponentEnabledSetting(ComponentName componentName, int newState, int flags);
    public abstract int getComponentEnabledSetting(ComponentName componentName);
    public abstract void setApplicationEnabledSetting(String packageName, int newState, int flags);
    public abstract int getApplicationEnabledSetting(String packageName);
    public abstract String getInstallerPackageName(String packageName);
    public InstallSourceInfo getInstallSourceInfo(String packageName) throws NameNotFoundException { return new InstallSourceInfo(); }
    public abstract CharSequence getApplicationLabel(ApplicationInfo info);
    public abstract Drawable getApplicationIcon(ApplicationInfo info);
    public abstract Drawable getApplicationIcon(String packageName) throws NameNotFoundException;
    public abstract CharSequence getText(String packageName, int resid, ApplicationInfo appInfo);
    public abstract android.content.res.Resources getResourcesForApplication(ApplicationInfo app) throws NameNotFoundException;
    public abstract android.content.res.Resources getResourcesForApplication(String packageName) throws NameNotFoundException;
    public abstract String[] getPackagesForUid(int uid);
    public abstract String getNameForUid(int uid);
    public abstract int getPackageUid(String packageName, int flags) throws NameNotFoundException;
    public abstract boolean isSafeMode();
    public abstract PermissionInfo getPermissionInfo(String permName, int flags) throws NameNotFoundException;
    public abstract int checkSignatures(String packageName1, String packageName2);
    public boolean isInstantApp() { return false; }
    public boolean isInstantApp(String packageName) { return false; }
    public boolean canRequestPackageInstalls() { return false; }
    public PackageInstaller getPackageInstaller() { return null; }
    public boolean isPackageSuspended() { return false; }
    public boolean isDeviceUpgrading() { return false; }
    public Intent getLeanbackLaunchIntentForPackage(String packageName) { return null; }
    public android.content.pm.ModuleInfo getModuleInfo(String packageName, int flags) throws NameNotFoundException { throw new NameNotFoundException(packageName); }
    public Drawable getDefaultActivityIcon() { return null; }
    public Drawable getActivityIcon(ComponentName activityName) throws NameNotFoundException { return null; }
    public CharSequence getUserBadgedLabel(CharSequence label, android.os.UserHandle user) { return label; }
}
