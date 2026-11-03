// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The vibrator of a device that has none: each vibration does nothing. */
public abstract class Vibrator {
    public static final int VIBRATION_EFFECT_SUPPORT_UNKNOWN = 0, VIBRATION_EFFECT_SUPPORT_YES = 1, VIBRATION_EFFECT_SUPPORT_NO = 2;

    public Vibrator() {}

    public boolean hasVibrator() { return false; }
    public boolean hasAmplitudeControl() { return false; }
    public int getId() { return -1; }
    @Deprecated public void vibrate(long milliseconds) {}
    @Deprecated public void vibrate(long[] pattern, int repeat) {}
    public void vibrate(VibrationEffect vibe) {}
    public void cancel() {}
    public float getResonantFrequency() { return Float.NaN; }
    public float getQFactor() { return Float.NaN; }
    public int areAllEffectsSupported(int... effectIds) { return VIBRATION_EFFECT_SUPPORT_NO; }
    public boolean areAllPrimitivesSupported(int... primitiveIds) { return false; }
}
