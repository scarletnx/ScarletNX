// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.credentials.CredentialOption;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;

/** The credential retrieval that the user picked, as its provider gets it. */
public final class GetCredentialRequest implements Parcelable {
    private final CallingAppInfo callingAppInfo;
    private final List<CredentialOption> options;

    public GetCredentialRequest(CallingAppInfo callingAppInfo, List<CredentialOption> credentialOptions) { this.callingAppInfo = callingAppInfo; options = credentialOptions; }
    public CallingAppInfo getCallingAppInfo() { return callingAppInfo; }
    public List<CredentialOption> getCredentialOptions() { return options; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<GetCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public GetCredentialRequest createFromParcel(Parcel in) { return (GetCredentialRequest) in.readValue(null); }
        @Override public GetCredentialRequest[] newArray(int size) { return new GetCredentialRequest[size]; }
    };
}
