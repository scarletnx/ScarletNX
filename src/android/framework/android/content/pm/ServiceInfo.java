// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A service of a package. */
public class ServiceInfo extends ComponentInfo implements android.os.Parcelable {
    public static final int FOREGROUND_SERVICE_TYPE_NONE = 0, FOREGROUND_SERVICE_TYPE_DATA_SYNC = 1,
        FOREGROUND_SERVICE_TYPE_MANIFEST = -1, FOREGROUND_SERVICE_TYPE_SHORT_SERVICE = 2048;
    public String permission;
    public int flags, foregroundServiceType;

    public ServiceInfo() {}
    public ServiceInfo(ServiceInfo o) { super(o); permission = o.permission; flags = o.flags; foregroundServiceType = o.foregroundServiceType; }
    public int getForegroundServiceType() { return foregroundServiceType; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
