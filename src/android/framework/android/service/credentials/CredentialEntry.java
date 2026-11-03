// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.app.slice.Slice;
import android.os.Parcel;
import android.os.Parcelable;

/** A credential that a provider offers for one option of a retrieval, in a slice. */
public final class CredentialEntry implements Parcelable {
    private final String optionId, type;
    private final Slice slice;

    public CredentialEntry(String beginGetCredentialOptionId, String type, Slice slice) { optionId = beginGetCredentialOptionId; this.type = type; this.slice = slice; }
    public CredentialEntry(BeginGetCredentialOption beginGetCredentialOption, Slice slice) { this(beginGetCredentialOption.getId(), beginGetCredentialOption.getType(), slice); }
    public String getBeginGetCredentialOptionId() { return optionId; }
    public String getType() { return type; }
    public Slice getSlice() { return slice; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CredentialEntry> CREATOR = new Parcelable.Creator<>() {
        @Override public CredentialEntry createFromParcel(Parcel in) { return (CredentialEntry) in.readValue(null); }
        @Override public CredentialEntry[] newArray(int size) { return new CredentialEntry[size]; }
    };
}
