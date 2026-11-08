// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/**
 * The device's vibrators, of which this one has none: no IDs, and a default vibrator without a motor, whose
 * vibrations do nothing (as the system's Vibrator here).
 */
public abstract class VibratorManager {
    public VibratorManager() {}

    public abstract int[] getVibratorIds();
    public abstract Vibrator getVibrator(int vibratorId);
    public abstract Vibrator getDefaultVibrator();
    public abstract void cancel();
    public final void vibrate(CombinedVibration effect) {}

    /** the device's: none, the default a vibrator without a motor */
    public static VibratorManager none(Vibrator defaultVibrator) {
        return new VibratorManager() {
            @Override public int[] getVibratorIds() { return new int[0]; }
            @Override public Vibrator getVibrator(int vibratorId) { return defaultVibrator; }
            @Override public Vibrator getDefaultVibrator() { return defaultVibrator; }
            @Override public void cancel() {}
        };
    }
}
