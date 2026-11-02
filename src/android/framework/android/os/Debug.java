// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The process's debugging and memory figures: no debugger, and the JVM's heap as the process's memory. */
public final class Debug {
    private Debug() {}

    public static class MemoryInfo {
        public int dalvikPss, dalvikPrivateDirty, dalvikSharedDirty, nativePss, nativePrivateDirty, nativeSharedDirty,
            otherPss, otherPrivateDirty, otherSharedDirty;
        public MemoryInfo() {}
        public int getTotalPss() { return dalvikPss + nativePss + otherPss; }
        public int getTotalPrivateDirty() { return dalvikPrivateDirty + nativePrivateDirty + otherPrivateDirty; }
        public int getTotalSharedDirty() { return dalvikSharedDirty + nativeSharedDirty + otherSharedDirty; }
        public int getTotalPrivateClean() { return 0; }
        public int getTotalSharedClean() { return 0; }
        public int getTotalSwappablePss() { return getTotalPss(); }
        public String getMemoryStat(String statName) { return null; }
        public java.util.Map<String, String> getMemoryStats() { return new java.util.HashMap<>(); }
    }

    public static boolean isDebuggerConnected() { return false; }
    public static boolean waitingForDebugger() { return false; }
    public static void waitForDebugger() {}
    /** in KiB, as Android's */
    public static void getMemoryInfo(MemoryInfo info) {
        Runtime r = Runtime.getRuntime();
        info.dalvikPss = info.dalvikPrivateDirty = (int) ((r.totalMemory() - r.freeMemory()) >> 10);
    }
    public static long getPss() { Runtime r = Runtime.getRuntime(); return (r.totalMemory() - r.freeMemory()) >> 10; }
    public static long getNativeHeapSize() { return 0; }
    public static long getNativeHeapAllocatedSize() { return 0; }
    public static long getNativeHeapFreeSize() { return 0; }
    public static void dumpHprofData(String fileName) throws java.io.IOException {
        throw new UnsupportedOperationException("snx: no heap dumps");
    }
    public static long threadCpuTimeNanos() {
        return java.lang.management.ManagementFactory.getThreadMXBean().getCurrentThreadCpuTime();
    }
    public static void startMethodTracing() {}
    public static void startMethodTracing(String tracePath) {}
    public static void stopMethodTracing() {}
}
