// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.usage;

/** Usage statistics: the app is in the active standby bucket. */
public final class UsageStatsManager {
    public static final int INTERVAL_DAILY = 0, INTERVAL_WEEKLY = 1, INTERVAL_MONTHLY = 2, INTERVAL_YEARLY = 3, INTERVAL_BEST = 4;
    public static final int STANDBY_BUCKET_ACTIVE = 10, STANDBY_BUCKET_WORKING_SET = 20, STANDBY_BUCKET_FREQUENT = 30,
        STANDBY_BUCKET_RARE = 40, STANDBY_BUCKET_RESTRICTED = 45;

    public int getAppStandbyBucket() { return STANDBY_BUCKET_ACTIVE; }
    public boolean isAppInactive(String packageName) { return false; }
}
