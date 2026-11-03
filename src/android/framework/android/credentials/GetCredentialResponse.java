// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Parcel;
import android.os.Parcelable;

/** The result of a credential retrieval. */
public final class GetCredentialResponse implements Parcelable {
    private final Credential credential;

    public GetCredentialResponse(Credential credential) { this.credential = credential; }
    public Credential getCredential() { return credential; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<GetCredentialResponse> CREATOR = new Parcelable.Creator<>() {
        @Override public GetCredentialResponse createFromParcel(Parcel in) { return (GetCredentialResponse) in.readValue(null); }
        @Override public GetCredentialResponse[] newArray(int size) { return new GetCredentialResponse[size]; }
    };
}
