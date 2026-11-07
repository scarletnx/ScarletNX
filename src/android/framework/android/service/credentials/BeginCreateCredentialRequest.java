// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A credential creation, as a provider sees it before the user picks. */
public class BeginCreateCredentialRequest implements Parcelable {
    private final String type;
    private final Bundle data;
    private final CallingAppInfo callingAppInfo;

    public BeginCreateCredentialRequest(String type, Bundle data, CallingAppInfo callingAppInfo) { this.type = type; this.data = data; this.callingAppInfo = callingAppInfo; }
    public BeginCreateCredentialRequest(String type, Bundle data) { this(type, data, null); }
    public CallingAppInfo getCallingAppInfo() { return callingAppInfo; }
    public String getType() { return type; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BeginCreateCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public BeginCreateCredentialRequest createFromParcel(Parcel in) { return (BeginCreateCredentialRequest) in.readValue(null); }
        @Override public BeginCreateCredentialRequest[] newArray(int size) { return new BeginCreateCredentialRequest[size]; }
    };
}
