// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.app.slice.Slice;
import android.os.Parcel;
import android.os.Parcelable;

/** An entry where a credential provider can create a credential, in a slice. */
public final class CreateEntry implements Parcelable {
    private final Slice slice;

    public CreateEntry(Slice slice) { this.slice = slice; }
    public Slice getSlice() { return slice; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CreateEntry> CREATOR = new Parcelable.Creator<>() {
        @Override public CreateEntry createFromParcel(Parcel in) { return (CreateEntry) in.readValue(null); }
        @Override public CreateEntry[] newArray(int size) { return new CreateEntry[size]; }
    };
}
