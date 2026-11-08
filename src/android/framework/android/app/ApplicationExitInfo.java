// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** Why a past process of the app ended: there are none. */
public final class ApplicationExitInfo {
    public static final int REASON_UNKNOWN = 0, REASON_EXIT_SELF = 1, REASON_SIGNALED = 2, REASON_LOW_MEMORY = 3,
        REASON_CRASH = 4, REASON_CRASH_NATIVE = 5, REASON_ANR = 6, REASON_USER_REQUESTED = 10;
    public int getReason() { return REASON_UNKNOWN; }
    public long getTimestamp() { return 0; }
    public int getPid() { return 0; }
    public String getDescription() { return null; }
    public String getProcessName() { return null; }
    public int getImportance() { return ActivityManager.RunningAppProcessInfo.IMPORTANCE_GONE; }
    public long getPss() { return 0; }
    public long getRss() { return 0; }
    public java.io.InputStream getTraceInputStream() throws java.io.IOException { return null; }
}
