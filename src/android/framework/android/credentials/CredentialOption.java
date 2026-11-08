// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.content.ComponentName;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.HashSet;
import java.util.Set;

/** One kind of credential that a retrieval asks for. */
public final class CredentialOption implements Parcelable {
    public static final String FLATTENED_REQUEST = "android.credentials.GetCredentialOption.FLATTENED_REQUEST_STRING";
    private final String type;
    private final Bundle retrievalData, queryData;
    private final boolean systemProviderRequired;
    private final Set<ComponentName> allowedProviders;

    private CredentialOption(Builder b) {
        type = b.type; retrievalData = b.retrievalData; queryData = b.queryData;
        systemProviderRequired = b.systemProviderRequired; allowedProviders = Set.copyOf(b.allowedProviders);
    }
    public String getType() { return type; }
    public Bundle getCredentialRetrievalData() { return retrievalData; }
    public Bundle getCandidateQueryData() { return queryData; }
    public boolean isSystemProviderRequired() { return systemProviderRequired; }
    public Set<ComponentName> getAllowedProviders() { return allowedProviders; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CredentialOption> CREATOR = new Parcelable.Creator<>() {
        @Override public CredentialOption createFromParcel(Parcel in) { return (CredentialOption) in.readValue(null); }
        @Override public CredentialOption[] newArray(int size) { return new CredentialOption[size]; }
    };

    public static final class Builder {
        private final String type;
        private final Bundle retrievalData, queryData;
        private boolean systemProviderRequired;
        private Set<ComponentName> allowedProviders = new HashSet<>();

        public Builder(String type, Bundle credentialRetrievalData, Bundle candidateQueryData) {
            this.type = type; retrievalData = credentialRetrievalData; queryData = candidateQueryData;
        }
        public Builder setIsSystemProviderRequired(boolean value) { systemProviderRequired = value; return this; }
        public Builder addAllowedProvider(ComponentName allowedProvider) { allowedProviders.add(allowedProvider); return this; }
        public Builder setAllowedProviders(Set<ComponentName> providers) { allowedProviders = new HashSet<>(providers); return this; }
        public CredentialOption build() { return new CredentialOption(this); }
    }
}
