// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/** A provider's entries where a credential can be created. */
public final class BeginCreateCredentialResponse implements Parcelable {
    private final List<CreateEntry> createEntries;
    private final RemoteEntry remoteCreateEntry;

    public BeginCreateCredentialResponse() { this(List.of(), null); }
    private BeginCreateCredentialResponse(List<CreateEntry> entries, RemoteEntry remote) { createEntries = entries; remoteCreateEntry = remote; }
    public List<CreateEntry> getCreateEntries() { return createEntries; }
    public RemoteEntry getRemoteCreateEntry() { return remoteCreateEntry; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BeginCreateCredentialResponse> CREATOR = new Parcelable.Creator<>() {
        @Override public BeginCreateCredentialResponse createFromParcel(Parcel in) { return (BeginCreateCredentialResponse) in.readValue(null); }
        @Override public BeginCreateCredentialResponse[] newArray(int size) { return new BeginCreateCredentialResponse[size]; }
    };

    public static final class Builder {
        private final List<CreateEntry> entries = new ArrayList<>();
        private RemoteEntry remote;
        public Builder() {}
        public Builder addCreateEntry(CreateEntry createEntry) { entries.add(createEntry); return this; }
        public Builder setCreateEntries(List<CreateEntry> createEntries) { entries.clear(); entries.addAll(createEntries); return this; }
        public Builder setRemoteCreateEntry(RemoteEntry remoteCreateEntry) { remote = remoteCreateEntry; return this; }
        public BeginCreateCredentialResponse build() { return new BeginCreateCredentialResponse(List.copyOf(entries), remote); }
    }
}
