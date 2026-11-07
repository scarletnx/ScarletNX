// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.app.slice.Slice;
import android.os.Parcel;
import android.os.Parcelable;

/** An entry of a credential provider on another device, in a slice. */
public final class RemoteEntry implements Parcelable {
    private final Slice slice;

    public RemoteEntry(Slice slice) { this.slice = slice; }
    public Slice getSlice() { return slice; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<RemoteEntry> CREATOR = new Parcelable.Creator<>() {
        @Override public RemoteEntry createFromParcel(Parcel in) { return (RemoteEntry) in.readValue(null); }
        @Override public RemoteEntry[] newArray(int size) { return new RemoteEntry[size]; }
    };
}
