// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.credentials;

import android.content.pm.SigningInfo;
import android.os.Parcel;
import android.os.Parcelable;

/** The app that asks a credential provider for a credential. */
public final class CallingAppInfo implements Parcelable {
    private final String packageName, origin;
    private final SigningInfo signingInfo;

    public CallingAppInfo(String packageName, SigningInfo signingInfo) { this(packageName, signingInfo, null); }
    public CallingAppInfo(String packageName, SigningInfo signingInfo, String origin) { this.packageName = packageName; this.signingInfo = signingInfo; this.origin = origin; }
    public String getPackageName() { return packageName; }
    public SigningInfo getSigningInfo() { return signingInfo; }
    public String getOrigin() { return origin; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CallingAppInfo> CREATOR = new Parcelable.Creator<>() {
        @Override public CallingAppInfo createFromParcel(Parcel in) { return (CallingAppInfo) in.readValue(null); }
        @Override public CallingAppInfo[] newArray(int size) { return new CallingAppInfo[size]; }
    };
}
