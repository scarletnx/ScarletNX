// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A package: its version and components. */
public class PackageInfo implements android.os.Parcelable {
    public String packageName, versionName, sharedUserId, requestedPermissions[], splitNames[];
    public int versionCode, sharedUserLabel, baseRevisionCode, requestedPermissionsFlags[];
    public long firstInstallTime, lastUpdateTime;
    public ApplicationInfo applicationInfo;
    public ActivityInfo[] activities, receivers;
    public ServiceInfo[] services;
    public ProviderInfo[] providers;
    public Signature[] signatures;
    public SigningInfo signingInfo;
    private long longVersionCode;

    public long getLongVersionCode() { return longVersionCode != 0 ? longVersionCode : versionCode; }
    public void setLongVersionCode(long longVersionCode) { this.longVersionCode = longVersionCode; versionCode = (int) longVersionCode; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
