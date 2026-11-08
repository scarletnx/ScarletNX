// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A vibration of several vibrators at once or one after the other: kept, as no vibrator plays it here. */
public abstract class CombinedVibration implements Parcelable {
    CombinedVibration() {}

    public static CombinedVibration createParallel(VibrationEffect effect) {
        if (effect == null) throw new IllegalArgumentException("effect must not be null");
        return new Mono(effect);
    }
    public static ParallelCombination startParallel() { return new ParallelCombination(); }
    public long getDuration() { return -1; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeValue(this); }

    private static final class Mono extends CombinedVibration {
        final VibrationEffect effect;
        Mono(VibrationEffect effect) { this.effect = effect; }
    }
    private static final class Stereo extends CombinedVibration {
        final android.util.SparseArray<VibrationEffect> effects;
        Stereo(android.util.SparseArray<VibrationEffect> effects) { this.effects = effects; }
    }
    public static final class ParallelCombination {
        private final android.util.SparseArray<VibrationEffect> effects = new android.util.SparseArray<>();
        ParallelCombination() {}
        public ParallelCombination addVibrator(int vibratorId, VibrationEffect effect) {
            effects.put(vibratorId, effect);
            return this;
        }
        public CombinedVibration combine() {
            if (effects.size() == 0) throw new IllegalStateException("Combination must have at least one element to combine.");
            return new Stereo(effects.clone());
        }
    }
    public static final Parcelable.Creator<CombinedVibration> CREATOR = new Parcelable.Creator<>() {
        @Override public CombinedVibration createFromParcel(Parcel in) { return (CombinedVibration) in.readValue(null); }
        @Override public CombinedVibration[] newArray(int size) { return new CombinedVibration[size]; }
    };
}
