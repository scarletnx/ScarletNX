// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** The result of a credential creation. */
public final class CreateCredentialResponse implements Parcelable {
    private final Bundle data;

    public CreateCredentialResponse(Bundle data) { this.data = data; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CreateCredentialResponse> CREATOR = new Parcelable.Creator<>() {
        @Override public CreateCredentialResponse createFromParcel(Parcel in) { return (CreateCredentialResponse) in.readValue(null); }
        @Override public CreateCredentialResponse[] newArray(int size) { return new CreateCredentialResponse[size]; }
    };
}
