// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os.storage;

import android.content.Context;
import android.content.Intent;
import android.os.Environment;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.File;

/** The one storage volume: the emulated shared storage of the device, in the app's data folder. */
public final class StorageVolume implements Parcelable {
    StorageVolume() {}

    public File getDirectory() { return Environment.getExternalStorageDirectory(); }
    public String getDescription(Context context) { return "Internal shared storage"; }
    public boolean isPrimary() { return true; }
    public boolean isRemovable() { return false; }
    public boolean isEmulated() { return true; }
    public String getUuid() { return null; }
    public String getMediaStoreVolumeName() { return "external_primary"; }
    public String getState() { return Environment.MEDIA_MOUNTED; }
    public Intent createOpenDocumentTreeIntent() { return new Intent("android.intent.action.OPEN_DOCUMENT_TREE"); }

    @Override public boolean equals(Object o) { return o instanceof StorageVolume; }
    @Override public int hashCode() { return 1; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {}
    public static final Parcelable.Creator<StorageVolume> CREATOR = new Parcelable.Creator<>() {
        @Override public StorageVolume createFromParcel(Parcel in) { return new StorageVolume(); }
        @Override public StorageVolume[] newArray(int size) { return new StorageVolume[size]; }
    };
}
