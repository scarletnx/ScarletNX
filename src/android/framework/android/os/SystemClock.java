// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** Android's clocks, on the host's monotonic clock. Deep sleep never happens: uptime and elapsed time are one. */
public final class SystemClock {
    private static final long START = System.nanoTime() - 60_000_000_000L; // as if booted a minute before

    private SystemClock() {}

    public static long uptimeMillis() { return (System.nanoTime() - START) / 1_000_000; }
    public static long uptimeNanos() { return System.nanoTime() - START; }
    public static long elapsedRealtime() { return uptimeMillis(); }
    public static long elapsedRealtimeNanos() { return uptimeNanos(); }
    public static long currentThreadTimeMillis() { return uptimeMillis(); }
    public static long currentThreadTimeMicro() { return uptimeNanos() / 1000; }
    public static long currentTimeMicro() { return System.currentTimeMillis() * 1000; }
    public static boolean setCurrentTimeMillis(long millis) { return false; }
    public static java.time.Clock currentNetworkTimeClock() { return java.time.Clock.systemUTC(); }
    public static java.time.Clock currentGnssTimeClock() { return java.time.Clock.systemUTC(); }

    public static void sleep(long ms) {
        long end = uptimeMillis() + ms;
        boolean interrupted = false;
        while (ms > 0) {
            try { Thread.sleep(ms); } catch (InterruptedException e) { interrupted = true; }
            ms = end - uptimeMillis();
        }
        if (interrupted) Thread.currentThread().interrupt();
    }
}
