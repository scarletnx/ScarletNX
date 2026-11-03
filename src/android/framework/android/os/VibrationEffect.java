// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A vibration: its timings and amplitudes. No vibrator plays it here. */
public abstract class VibrationEffect implements Parcelable {
    public static final int DEFAULT_AMPLITUDE = -1;
    public static final int EFFECT_CLICK = 0, EFFECT_DOUBLE_CLICK = 1, EFFECT_TICK = 2, EFFECT_HEAVY_CLICK = 5;

    VibrationEffect() {}

    private static final class Waveform extends VibrationEffect {
        final long[] timings;
        final int[] amplitudes;
        final int repeat;

        Waveform(long[] timings, int[] amplitudes, int repeat) { this.timings = timings; this.amplitudes = amplitudes; this.repeat = repeat; }

        @Override public long getDuration() {
            if (repeat >= 0) return Long.MAX_VALUE;
            long total = 0;
            for (long t : timings) total += t;
            return total;
        }
    }

    private static final class Predefined extends VibrationEffect {
        final int id;
        Predefined(int id) { this.id = id; }
        @Override public long getDuration() { return -1; }
    }

    public static VibrationEffect createOneShot(long milliseconds, int amplitude) {
        if (milliseconds <= 0) throw new IllegalArgumentException("Timing must be greater than zero.");
        checkAmplitude(amplitude);
        return new Waveform(new long[] { milliseconds }, new int[] { amplitude }, -1);
    }

    public static VibrationEffect createWaveform(long[] timings, int repeat) {
        int[] amplitudes = new int[timings.length];
        for (int i = 0; i < amplitudes.length; ++i) amplitudes[i] = i % 2 == 0 ? 0 : DEFAULT_AMPLITUDE;
        return createWaveform(timings, amplitudes, repeat);
    }

    public static VibrationEffect createWaveform(long[] timings, int[] amplitudes, int repeat) {
        if (timings.length == 0 || timings.length != amplitudes.length) throw new IllegalArgumentException("Timing and amplitude arrays must be of equal length and not empty.");
        if (repeat < -1 || repeat >= timings.length) throw new IllegalArgumentException("Repeat index must be within the bounds of the timings array.");
        for (long t : timings) if (t < 0) throw new IllegalArgumentException("Timings must all be non-negative.");
        for (int a : amplitudes) if (a != 0) checkAmplitude(a);
        return new Waveform(timings.clone(), amplitudes.clone(), repeat);
    }

    public static VibrationEffect createPredefined(int effectId) { return new Predefined(effectId); }

    private static void checkAmplitude(int amplitude) {
        if (amplitude != DEFAULT_AMPLITUDE && (amplitude < 1 || amplitude > 255)) {
            throw new IllegalArgumentException("Amplitude must either be DEFAULT_AMPLITUDE, or between 1 and 255 inclusive.");
        }
    }

    public abstract long getDuration();

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<VibrationEffect> CREATOR = new Parcelable.Creator<>() {
        @Override public VibrationEffect createFromParcel(Parcel in) { return (VibrationEffect) in.readValue(null); }
        @Override public VibrationEffect[] newArray(int size) { return new VibrationEffect[size]; }
    };
}
