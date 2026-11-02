// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A content provider of a package. */
public final class ProviderInfo extends ComponentInfo implements android.os.Parcelable {
    public String authority, readPermission, writePermission;
    public boolean grantUriPermissions, forceUriPermissions, multiprocess, isSyncable;
    public int initOrder, flags;

    public ProviderInfo() {}
    public ProviderInfo(ProviderInfo o) {
        super(o);
        authority = o.authority; readPermission = o.readPermission; writePermission = o.writePermission;
        grantUriPermissions = o.grantUriPermissions; multiprocess = o.multiprocess; initOrder = o.initOrder; flags = o.flags;
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
