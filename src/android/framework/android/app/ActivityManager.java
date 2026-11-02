// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.Collections;
import java.util.List;

/** The process's state, as the activity manager reports it: one foreground app with memory to spare. */
public class ActivityManager {
    public static final int RECENT_WITH_EXCLUDED = 1;

    public static class RunningAppProcessInfo {
        public static final int IMPORTANCE_FOREGROUND = 100, IMPORTANCE_VISIBLE = 200, IMPORTANCE_SERVICE = 300,
            IMPORTANCE_CACHED = 400, IMPORTANCE_GONE = 1000;
        public String processName;
        public int pid, uid, importance = IMPORTANCE_FOREGROUND, lastTrimLevel, importanceReasonCode;
        public String[] pkgList;
        public RunningAppProcessInfo() {}
        public RunningAppProcessInfo(String processName, int pid, String[] pkgList) { this.processName = processName; this.pid = pid; this.pkgList = pkgList; }
    }

    public static class MemoryInfo {
        public long availMem, totalMem, threshold;
        public boolean lowMemory;
    }

    public static class TaskDescription {
        public TaskDescription() {}
        public TaskDescription(String label) {}
        public static class Builder {
            public Builder setLabel(String label) { return this; }
            public Builder setPrimaryColor(int color) { return this; }
            public TaskDescription build() { return new TaskDescription(); }
        }
    }

    public List<RunningAppProcessInfo> getRunningAppProcesses() {
        RunningAppProcessInfo i = new RunningAppProcessInfo(snx.android.Main.processName(), android.os.Process.myPid(), new String[] {snx.android.Main.processName()});
        i.uid = android.os.Process.myUid();
        return Collections.singletonList(i);
    }
    public static void getMyMemoryState(RunningAppProcessInfo outState) {
        outState.processName = snx.android.Main.processName();
        outState.pid = android.os.Process.myPid();
        outState.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
    }
    public void getMemoryInfo(MemoryInfo outInfo) {
        Runtime r = Runtime.getRuntime();
        outInfo.totalMem = 8L << 30;
        outInfo.availMem = 4L << 30;
        outInfo.threshold = 256L << 20;
        outInfo.lowMemory = false;
    }
    public int getMemoryClass() { return (int) Math.min(512, Runtime.getRuntime().maxMemory() >> 20); }
    public int getLargeMemoryClass() { return getMemoryClass(); }
    public boolean isLowRamDevice() { return false; }
    public static boolean isRunningInUserTestHarness() { return false; }
    public static boolean isUserAMonkey() { return false; }
    public boolean isBackgroundRestricted() { return false; }
    public boolean clearApplicationUserData() { return false; }
    public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String packageName, int pid, int maxNum) { return Collections.emptyList(); }
    public List<Object> getAppTasks() { return Collections.emptyList(); }
    public int getLauncherLargeIconDensity() { return snx.android.Device.metrics().densityDpi; }
    public int getLauncherLargeIconSize() { return 48; }
}
