// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** The application of a package. */
public class ApplicationInfo extends PackageItemInfo implements android.os.Parcelable {
    public static final int FLAG_SYSTEM = 1, FLAG_DEBUGGABLE = 2, FLAG_HAS_CODE = 4, FLAG_PERSISTENT = 8,
        FLAG_FACTORY_TEST = 16, FLAG_ALLOW_TASK_REPARENTING = 32, FLAG_ALLOW_CLEAR_USER_DATA = 64,
        FLAG_UPDATED_SYSTEM_APP = 128, FLAG_TEST_ONLY = 256, FLAG_SUPPORTS_SMALL_SCREENS = 512,
        FLAG_SUPPORTS_NORMAL_SCREENS = 1024, FLAG_SUPPORTS_LARGE_SCREENS = 2048, FLAG_RESIZEABLE_FOR_SCREENS = 4096,
        FLAG_SUPPORTS_SCREEN_DENSITIES = 8192, FLAG_VM_SAFE_MODE = 16384, FLAG_ALLOW_BACKUP = 32768,
        FLAG_KILL_AFTER_RESTORE = 65536, FLAG_RESTORE_ANY_VERSION = 131072, FLAG_EXTERNAL_STORAGE = 262144,
        FLAG_SUPPORTS_XLARGE_SCREENS = 524288, FLAG_LARGE_HEAP = 1048576, FLAG_STOPPED = 2097152,
        FLAG_SUPPORTS_RTL = 4194304, FLAG_INSTALLED = 8388608, FLAG_IS_DATA_ONLY = 16777216, FLAG_IS_GAME = 33554432,
        FLAG_FULL_BACKUP_ONLY = 67108864, FLAG_USES_CLEARTEXT_TRAFFIC = 134217728, FLAG_EXTRACT_NATIVE_LIBS = 268435456,
        FLAG_HARDWARE_ACCELERATED = 536870912, FLAG_SUSPENDED = 1073741824, FLAG_MULTIARCH = 0x80000000;
    public static final int CATEGORY_UNDEFINED = -1;
    public String taskAffinity, permission, processName, className, backupAgentName, manageSpaceActivityName,
        sourceDir, publicSourceDir, nativeLibraryDir, dataDir, deviceProtectedDataDir, appComponentFactory, splitNames[],
        splitSourceDirs[], splitPublicSourceDirs[], sharedLibraryFiles[];
    public int theme, flags = FLAG_HAS_CODE | FLAG_INSTALLED | FLAG_SUPPORTS_RTL | FLAG_HARDWARE_ACCELERATED,
        descriptionRes, uid = android.os.Process.APP_UID, minSdkVersion, targetSdkVersion, compileSdkVersion,
        category = CATEGORY_UNDEFINED, requiresSmallestWidthDp, compatibleWidthLimitDp, largestWidthLimitDp;
    public String compileSdkVersionCodename;
    public boolean enabled = true;
    public int uiOptions;

    public ApplicationInfo() {}
    public ApplicationInfo(ApplicationInfo o) {
        super(o);
        taskAffinity = o.taskAffinity; permission = o.permission; processName = o.processName; className = o.className;
        sourceDir = o.sourceDir; publicSourceDir = o.publicSourceDir; nativeLibraryDir = o.nativeLibraryDir; dataDir = o.dataDir;
        deviceProtectedDataDir = o.deviceProtectedDataDir; appComponentFactory = o.appComponentFactory; theme = o.theme; flags = o.flags;
        uid = o.uid; minSdkVersion = o.minSdkVersion; targetSdkVersion = o.targetSdkVersion; enabled = o.enabled; category = o.category;
    }
    public boolean isProfileableByShell() { return false; }
    public boolean isResourceOverlay() { return false; }
    public int getGwpAsanMode() { return -1; }
    public int getMemtagMode() { return -1; }
    public boolean areAttributionsUserVisible() { return false; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
