// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.common;

import android.os.Parcel;
import android.os.Parcelable;

/** Signals for ad selection, a JSON string. */
public final class AdSelectionSignals implements Parcelable {
    public static final AdSelectionSignals EMPTY = fromString("{}");
    private final String signals;

    private AdSelectionSignals(String signals) { this.signals = signals; }
    public static AdSelectionSignals fromString(String source) {
        if (source == null) throw new NullPointerException("source");
        return new AdSelectionSignals(source);
    }
    @Override public String toString() { return signals; }
    @Override public boolean equals(Object o) { return o instanceof AdSelectionSignals s && signals.equals(s.signals); }
    @Override public int hashCode() { return signals.hashCode(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<AdSelectionSignals> CREATOR = new Parcelable.Creator<>() {
        @Override public AdSelectionSignals createFromParcel(Parcel in) { return (AdSelectionSignals) in.readValue(null); }
        @Override public AdSelectionSignals[] newArray(int size) { return new AdSelectionSignals[size]; }
    };
}
