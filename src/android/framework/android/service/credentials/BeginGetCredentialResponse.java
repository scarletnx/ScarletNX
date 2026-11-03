// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/** A provider's credentials and actions for a retrieval. */
public final class BeginGetCredentialResponse implements Parcelable {
    private final List<CredentialEntry> credentialEntries;
    private final List<Action> actions, authenticationActions;
    private final RemoteEntry remoteCredentialEntry;

    public BeginGetCredentialResponse() { this(List.of(), List.of(), List.of(), null); }
    private BeginGetCredentialResponse(List<CredentialEntry> entries, List<Action> actions, List<Action> authenticationActions, RemoteEntry remote) {
        credentialEntries = entries; this.actions = actions; this.authenticationActions = authenticationActions; remoteCredentialEntry = remote;
    }
    public List<CredentialEntry> getCredentialEntries() { return credentialEntries; }
    public List<Action> getActions() { return actions; }
    public List<Action> getAuthenticationActions() { return authenticationActions; }
    public RemoteEntry getRemoteCredentialEntry() { return remoteCredentialEntry; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BeginGetCredentialResponse> CREATOR = new Parcelable.Creator<>() {
        @Override public BeginGetCredentialResponse createFromParcel(Parcel in) { return (BeginGetCredentialResponse) in.readValue(null); }
        @Override public BeginGetCredentialResponse[] newArray(int size) { return new BeginGetCredentialResponse[size]; }
    };

    public static final class Builder {
        private final List<CredentialEntry> entries = new ArrayList<>();
        private final List<Action> actions = new ArrayList<>(), authenticationActions = new ArrayList<>();
        private RemoteEntry remote;
        public Builder() {}
        public Builder addCredentialEntry(CredentialEntry credentialEntry) { entries.add(credentialEntry); return this; }
        public Builder addAction(Action action) { actions.add(action); return this; }
        public Builder addAuthenticationAction(Action authenticationAction) { authenticationActions.add(authenticationAction); return this; }
        public Builder setRemoteCredentialEntry(RemoteEntry remoteCredentialEntry) { remote = remoteCredentialEntry; return this; }
        public BeginGetCredentialResponse build() { return new BeginGetCredentialResponse(List.copyOf(entries), List.copyOf(actions), List.copyOf(authenticationActions), remote); }
    }
}
