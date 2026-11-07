// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.common;

import android.os.Parcel;
import android.os.Parcelable;

/** The identifier of an ad tech, its domain. */
public final class AdTechIdentifier implements Parcelable {
    private final String id;

    private AdTechIdentifier(String id) { this.id = id; }
    public static AdTechIdentifier fromString(String source) {
        if (source == null) throw new NullPointerException("source");
        return new AdTechIdentifier(source);
    }
    @Override public String toString() { return id; }
    @Override public boolean equals(Object o) { return o instanceof AdTechIdentifier t && id.equals(t.id); }
    @Override public int hashCode() { return id.hashCode(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<AdTechIdentifier> CREATOR = new Parcelable.Creator<>() {
        @Override public AdTechIdentifier createFromParcel(Parcel in) { return (AdTechIdentifier) in.readValue(null); }
        @Override public AdTechIdentifier[] newArray(int size) { return new AdTechIdentifier[size]; }
    };
}
