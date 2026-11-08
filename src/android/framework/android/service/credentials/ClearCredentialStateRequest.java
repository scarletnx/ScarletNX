// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A request to a provider to clear its credential state for an app. */
public final class ClearCredentialStateRequest implements Parcelable {
    private final CallingAppInfo callingAppInfo;
    private final Bundle data;

    public ClearCredentialStateRequest(CallingAppInfo callingAppInfo, Bundle data) { this.callingAppInfo = callingAppInfo; this.data = data; }
    public CallingAppInfo getCallingAppInfo() { return callingAppInfo; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<ClearCredentialStateRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public ClearCredentialStateRequest createFromParcel(Parcel in) { return (ClearCredentialStateRequest) in.readValue(null); }
        @Override public ClearCredentialStateRequest[] newArray(int size) { return new ClearCredentialStateRequest[size]; }
    };
}
