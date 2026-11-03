// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** The credential creation that the user picked, as its provider gets it. */
public final class CreateCredentialRequest implements Parcelable {
    private final CallingAppInfo callingAppInfo;
    private final String type;
    private final Bundle data;

    public CreateCredentialRequest(CallingAppInfo callingAppInfo, String type, Bundle data) { this.callingAppInfo = callingAppInfo; this.type = type; this.data = data; }
    public CallingAppInfo getCallingAppInfo() { return callingAppInfo; }
    public String getType() { return type; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CreateCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public CreateCredentialRequest createFromParcel(Parcel in) { return (CreateCredentialRequest) in.readValue(null); }
        @Override public CreateCredentialRequest[] newArray(int size) { return new CreateCredentialRequest[size]; }
    };
}
