// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.app.slice.Slice;
import android.os.Parcel;
import android.os.Parcelable;

/** An action that a credential provider shows to the user, in a slice. */
public final class Action implements Parcelable {
    private final Slice slice;

    public Action(Slice slice) { this.slice = slice; }
    public Slice getSlice() { return slice; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<Action> CREATOR = new Parcelable.Creator<>() {
        @Override public Action createFromParcel(Parcel in) { return (Action) in.readValue(null); }
        @Override public Action[] newArray(int size) { return new Action[size]; }
    };
}
