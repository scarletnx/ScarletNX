// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.admin;

import android.content.ComponentName;
import java.util.List;

/** Device policy: an unmanaged device with no active administrator, no owner and no restriction. */
public class DevicePolicyManager {
    public static final String ACTION_ADD_DEVICE_ADMIN = "android.app.action.ADD_DEVICE_ADMIN",
        ACTION_SET_NEW_PASSWORD = "android.app.action.SET_NEW_PASSWORD", ACTION_START_ENCRYPTION = "android.app.action.START_ENCRYPTION",
        EXTRA_DEVICE_ADMIN = "android.app.extra.DEVICE_ADMIN", EXTRA_ADD_EXPLANATION = "android.app.extra.ADD_EXPLANATION";
    public static final int ENCRYPTION_STATUS_UNSUPPORTED = 0, ENCRYPTION_STATUS_INACTIVE = 1, ENCRYPTION_STATUS_ACTIVATING = 2,
        ENCRYPTION_STATUS_ACTIVE = 3, ENCRYPTION_STATUS_ACTIVE_DEFAULT_KEY = 4, ENCRYPTION_STATUS_ACTIVE_PER_USER = 5;
    public static final int PASSWORD_QUALITY_UNSPECIFIED = 0;

    public boolean isAdminActive(ComponentName admin) { return false; }
    /** Null: no administrator is active. */
    public List<ComponentName> getActiveAdmins() { return null; }
    public boolean isDeviceOwnerApp(String packageName) { return false; }
    public boolean isProfileOwnerApp(String packageName) { return false; }
    public boolean isDeviceManaged() { return false; }
    public boolean isLockTaskPermitted(String packageName) { return false; }
    public boolean getCameraDisabled(ComponentName admin) { return false; }
    public boolean getScreenCaptureDisabled(ComponentName admin) { return false; }
    public int getStorageEncryptionStatus() { return ENCRYPTION_STATUS_UNSUPPORTED; }
    public int getPasswordQuality(ComponentName admin) { return PASSWORD_QUALITY_UNSPECIFIED; }
    public boolean isActivePasswordSufficient() { return true; }
    public int getKeyguardDisabledFeatures(ComponentName admin) { return 0; }
    public android.os.Bundle getApplicationRestrictions(ComponentName admin, String packageName) { return new android.os.Bundle(); }
}
