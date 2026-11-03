// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A request to clear the credential state of the user's providers. */
public final class ClearCredentialStateRequest implements Parcelable {
    private final Bundle data;

    public ClearCredentialStateRequest(Bundle data) { this.data = data; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<ClearCredentialStateRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public ClearCredentialStateRequest createFromParcel(Parcel in) { return (ClearCredentialStateRequest) in.readValue(null); }
        @Override public ClearCredentialStateRequest[] newArray(int size) { return new ClearCredentialStateRequest[size]; }
    };
}
