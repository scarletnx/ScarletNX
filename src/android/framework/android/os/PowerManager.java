// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** Power: the screen is on, nothing saves power, and wake locks hold nothing. */
public final class PowerManager {
    public static final int PARTIAL_WAKE_LOCK = 0x00000001;
    public static final int SCREEN_DIM_WAKE_LOCK = 0x00000006;
    public static final int SCREEN_BRIGHT_WAKE_LOCK = 0x0000000a;
    public static final int FULL_WAKE_LOCK = 0x0000001a;
    public static final int ACQUIRE_CAUSES_WAKEUP = 0x10000000;
    public static final int ON_AFTER_RELEASE = 0x20000000;
    public static final int THERMAL_STATUS_NONE = 0;
    public static final String ACTION_POWER_SAVE_MODE_CHANGED = "android.os.action.POWER_SAVE_MODE_CHANGED";
    public static final String ACTION_DEVICE_IDLE_MODE_CHANGED = "android.os.action.DEVICE_IDLE_MODE_CHANGED";

    public PowerManager() {}

    public final class WakeLock {
        private int count;
        private boolean refCounted = true;
        private final String tag;

        WakeLock(String tag) { this.tag = tag; }

        public void setReferenceCounted(boolean value) { refCounted = value; }
        public void acquire() { synchronized (this) { count = refCounted ? count + 1 : 1; } }
        public void acquire(long timeout) { acquire(); }
        public void release() { release(0); }
        public void release(int flags) { synchronized (this) { if (count > 0) count = refCounted ? count - 1 : 0; } }
        public boolean isHeld() { synchronized (this) { return count > 0; } }
        public void setWorkSource(Object ws) {}
        @Override public String toString() { return "WakeLock{" + tag + " held=" + isHeld() + "}"; }
    }

    public WakeLock newWakeLock(int levelAndFlags, String tag) { return new WakeLock(tag); }
    public boolean isScreenOn() { return true; }
    public boolean isInteractive() { return true; }
    public boolean isPowerSaveMode() { return false; }
    public boolean isDeviceIdleMode() { return false; }
    public boolean isLowPowerStandbyEnabled() { return false; }
    public boolean isIgnoringBatteryOptimizations(String packageName) { return true; }
    public boolean isSustainedPerformanceModeSupported() { return false; }
    public int getCurrentThermalStatus() { return THERMAL_STATUS_NONE; }
    public float getThermalHeadroom(int forecastSeconds) { return Float.NaN; }
}
