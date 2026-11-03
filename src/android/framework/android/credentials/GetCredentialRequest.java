// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/** A request to get a credential, with the kinds of credential that it accepts. */
public final class GetCredentialRequest implements Parcelable {
    private final List<CredentialOption> options;
    private final Bundle data;
    private final String origin;

    private GetCredentialRequest(List<CredentialOption> options, Bundle data, String origin) { this.options = options; this.data = data; this.origin = origin; }
    public List<CredentialOption> getCredentialOptions() { return options; }
    public Bundle getData() { return data; }
    public String getOrigin() { return origin; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<GetCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public GetCredentialRequest createFromParcel(Parcel in) { return (GetCredentialRequest) in.readValue(null); }
        @Override public GetCredentialRequest[] newArray(int size) { return new GetCredentialRequest[size]; }
    };

    public static final class Builder {
        private final Bundle data;
        private final List<CredentialOption> options = new ArrayList<>();
        private String origin;

        public Builder(Bundle data) { this.data = data; }
        public Builder addCredentialOption(CredentialOption credentialOption) { options.add(credentialOption); return this; }
        public Builder setCredentialOptions(List<CredentialOption> credentialOptions) { options.clear(); options.addAll(credentialOptions); return this; }
        public Builder setOrigin(String origin) { this.origin = origin; return this; }
        public GetCredentialRequest build() { return new GetCredentialRequest(List.copyOf(options), data, origin); }
    }
}
