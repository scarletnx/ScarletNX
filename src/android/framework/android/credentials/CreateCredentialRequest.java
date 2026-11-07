// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A request to create a credential. */
public final class CreateCredentialRequest implements Parcelable {
    private final String type, origin;
    private final Bundle credentialData, candidateQueryData;
    private final boolean systemProviderRequired, alwaysSendAppInfo;

    private CreateCredentialRequest(Builder b) {
        type = b.type; origin = b.origin; credentialData = b.credentialData; candidateQueryData = b.candidateQueryData;
        systemProviderRequired = b.systemProviderRequired; alwaysSendAppInfo = b.alwaysSendAppInfo;
    }
    public String getType() { return type; }
    public Bundle getCredentialData() { return credentialData; }
    public Bundle getCandidateQueryData() { return candidateQueryData; }
    public boolean isSystemProviderRequired() { return systemProviderRequired; }
    public boolean alwaysSendAppInfoToProvider() { return alwaysSendAppInfo; }
    public String getOrigin() { return origin; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CreateCredentialRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public CreateCredentialRequest createFromParcel(Parcel in) { return (CreateCredentialRequest) in.readValue(null); }
        @Override public CreateCredentialRequest[] newArray(int size) { return new CreateCredentialRequest[size]; }
    };

    public static final class Builder {
        private final String type;
        private final Bundle credentialData, candidateQueryData;
        private String origin;
        private boolean systemProviderRequired, alwaysSendAppInfo;

        public Builder(String type, Bundle credentialData, Bundle candidateQueryData) {
            this.type = type; this.credentialData = credentialData; this.candidateQueryData = candidateQueryData;
        }
        public Builder setAlwaysSendAppInfoToProvider(boolean value) { alwaysSendAppInfo = value; return this; }
        public Builder setIsSystemProviderRequired(boolean value) { systemProviderRequired = value; return this; }
        public Builder setOrigin(String origin) { this.origin = origin; return this; }
        public CreateCredentialRequest build() { return new CreateCredentialRequest(this); }
    }
}
