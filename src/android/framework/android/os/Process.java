// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The app's process: the host's, as one app's uid. */
public class Process {
    public static final int FIRST_APPLICATION_UID = 10000;
    public static final int SYSTEM_UID = 1000;
    public static final int ROOT_UID = 0;
    public static final int SHELL_UID = 2000;
    public static final int PHONE_UID = 1001;
    public static final int BLUETOOTH_UID = 1002;
    public static final int INVALID_UID = -1;
    public static final int SIGNAL_QUIT = 3;
    public static final int SIGNAL_KILL = 9;
    public static final int SIGNAL_USR1 = 10;
    public static final int THREAD_PRIORITY_DEFAULT = 0;
    public static final int THREAD_PRIORITY_LOWEST = 19;
    public static final int THREAD_PRIORITY_BACKGROUND = 10;
    public static final int THREAD_PRIORITY_FOREGROUND = -2;
    public static final int THREAD_PRIORITY_DISPLAY = -4;
    public static final int THREAD_PRIORITY_URGENT_DISPLAY = -8;
    public static final int THREAD_PRIORITY_VIDEO = -10;
    public static final int THREAD_PRIORITY_AUDIO = -16;
    public static final int THREAD_PRIORITY_URGENT_AUDIO = -19;
    public static final int THREAD_PRIORITY_MORE_FAVORABLE = -1;
    public static final int THREAD_PRIORITY_LESS_FAVORABLE = 1;

    /** The app's uid: the first application uid, as a single installed app. */
    public static final int APP_UID = 10123;

    public static int myPid() { return (int) ProcessHandle.current().pid(); }
    public static int myTid() { return (int) Thread.currentThread().threadId(); }
    public static int myUid() { return APP_UID; }
    public static UserHandle myUserHandle() { return UserHandle.of(0); }
    public static boolean isApplicationUid(int uid) { return uid % 100000 >= FIRST_APPLICATION_UID; }
    public static boolean isIsolated() { return false; }
    public static boolean isSdkSandbox() { return false; }
    public static boolean is64Bit() { return true; }
    public static long getStartElapsedRealtime() { return 0; }
    public static long getStartUptimeMillis() { return 0; }
    public static String myProcessName() { return snx.android.Main.processName(); }
    public static void setThreadPriority(int tid, int priority) {}
    public static void setThreadPriority(int priority) {}
    public static int getThreadPriority(int tid) { return THREAD_PRIORITY_DEFAULT; }
    public static void killProcess(int pid) { if (pid == myPid()) System.exit(0); }
    public static void sendSignal(int pid, int signal) {
        if (pid == myPid() && signal == SIGNAL_KILL) Runtime.getRuntime().halt(137);
    }
    public static long getElapsedCpuTime() { return SystemClock.uptimeMillis(); }
    public static int getUidForName(String name) { return -1; }
    public static int getGidForName(String name) { return -1; }
}
