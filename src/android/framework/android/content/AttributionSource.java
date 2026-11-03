// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.os.Parcel;
import android.os.Parcelable;

/** Who accesses data: the app's own uid, package and attribution tag. */
public final class AttributionSource implements Parcelable {
    private final int uid;
    private final String packageName, attributionTag;

    private AttributionSource(int uid, String packageName, String attributionTag) {
        this.uid = uid;
        this.packageName = packageName;
        this.attributionTag = attributionTag;
    }

    public int getUid() { return uid; }
    public int getPid() { return android.os.Process.myPid(); }
    public String getPackageName() { return packageName; }
    public String getAttributionTag() { return attributionTag; }
    public AttributionSource getNext() { return null; }
    public boolean isTrusted(Context context) { return true; }

    @Override public boolean equals(Object o) {
        return o instanceof AttributionSource a && a.uid == uid && java.util.Objects.equals(a.packageName, packageName)
            && java.util.Objects.equals(a.attributionTag, attributionTag);
    }
    @Override public int hashCode() { return java.util.Objects.hash(uid, packageName, attributionTag); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeInt(uid); dest.writeString(packageName); dest.writeString(attributionTag); }
    public static final Parcelable.Creator<AttributionSource> CREATOR = new Parcelable.Creator<>() {
        @Override public AttributionSource createFromParcel(Parcel in) { return new AttributionSource(in.readInt(), in.readString(), in.readString()); }
        @Override public AttributionSource[] newArray(int size) { return new AttributionSource[size]; }
    };

    public static final class Builder {
        private final int uid;
        private String packageName, attributionTag;

        public Builder(int uid) { this.uid = uid; }
        public Builder(AttributionSource current) { uid = current.uid; packageName = current.packageName; attributionTag = current.attributionTag; }
        public Builder setPackageName(String value) { packageName = value; return this; }
        public Builder setAttributionTag(String value) { attributionTag = value; return this; }
        public Builder setPid(int value) { return this; }
        public Builder setNext(AttributionSource value) { return this; }
        public AttributionSource build() { return new AttributionSource(uid, packageName, attributionTag); }
    }
}
