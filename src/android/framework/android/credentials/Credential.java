// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A credential that a provider gave: its type and data. */
public final class Credential implements Parcelable {
    public static final String TYPE_PASSWORD_CREDENTIAL = "android.credentials.TYPE_PASSWORD_CREDENTIAL";
    private final String type;
    private final Bundle data;

    public Credential(String type, Bundle data) { this.type = type; this.data = data; }
    public String getType() { return type; }
    public Bundle getData() { return data; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<Credential> CREATOR = new Parcelable.Creator<>() {
        @Override public Credential createFromParcel(Parcel in) { return (Credential) in.readValue(null); }
        @Override public Credential[] newArray(int size) { return new Credential[size]; }
    };
}
