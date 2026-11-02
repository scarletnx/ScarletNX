// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** Network statistics: tags kept per thread, and counts the device doesn't keep (UNSUPPORTED), as on an emulator. */
public class TrafficStats {
    public static final long UNSUPPORTED = -1;
    private static final ThreadLocal<Integer> tag = ThreadLocal.withInitial(() -> -1);

    public TrafficStats() {}

    public static void setThreadStatsTag(int t) { tag.set(t); }
    public static int getAndSetThreadStatsTag(int t) { int old = tag.get(); tag.set(t); return old; }
    public static int getThreadStatsTag() { return tag.get(); }
    public static void clearThreadStatsTag() { tag.set(-1); }
    public static void tagSocket(java.net.Socket socket) {}
    public static void untagSocket(java.net.Socket socket) {}
    public static void tagDatagramSocket(java.net.DatagramSocket socket) {}
    public static void untagDatagramSocket(java.net.DatagramSocket socket) {}
    public static void incrementOperationCount(int operationCount) {}
    public static void incrementOperationCount(int tag, int operationCount) {}
    public static long getMobileRxBytes() { return UNSUPPORTED; }
    public static long getMobileTxBytes() { return UNSUPPORTED; }
    public static long getTotalRxBytes() { return UNSUPPORTED; }
    public static long getTotalTxBytes() { return UNSUPPORTED; }
    public static long getUidRxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidTxBytes(int uid) { return UNSUPPORTED; }
}
