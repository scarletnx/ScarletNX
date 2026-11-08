// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** PackageInstaller: nothing installs here. */
public class PackageInstaller {
    public String getInstallingPackageName() { return null; }
    public String getInitiatingPackageName() { return null; }
    public String getOriginatingPackageName() { return null; }
    /** No install session exists. */
    public java.util.List<SessionInfo> getAllSessions() { return new java.util.ArrayList<>(); }

    public static class SessionInfo implements android.os.Parcelable {
        String appPackageName;
        public String getAppPackageName() { return appPackageName; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
        public static final android.os.Parcelable.Creator<SessionInfo> CREATOR = new android.os.Parcelable.Creator<>() {
            @Override public SessionInfo createFromParcel(android.os.Parcel in) { return (SessionInfo) in.readValue(null); }
            @Override public SessionInfo[] newArray(int size) { return new SessionInfo[size]; }
        };
    }
}
