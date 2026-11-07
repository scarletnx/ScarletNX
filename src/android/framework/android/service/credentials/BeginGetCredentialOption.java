// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** One option of a credential retrieval, as a provider sees it before the user picks. */
public class BeginGetCredentialOption implements Parcelable {
    private final String id, type;
    private final Bundle candidateQueryData;

    public BeginGetCredentialOption(String id, String type, Bundle candidateQueryData) { this.id = id; this.type = type; this.candidateQueryData = candidateQueryData; }
    public String getId() { return id; }
    public String getType() { return type; }
    public Bundle getCandidateQueryData() { return candidateQueryData; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BeginGetCredentialOption> CREATOR = new Parcelable.Creator<>() {
        @Override public BeginGetCredentialOption createFromParcel(Parcel in) { return (BeginGetCredentialOption) in.readValue(null); }
        @Override public BeginGetCredentialOption[] newArray(int size) { return new BeginGetCredentialOption[size]; }
    };
}
