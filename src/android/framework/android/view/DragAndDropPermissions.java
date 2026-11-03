// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Permissions to the URIs of a drop. No other app drops here, so nothing is held. */
public final class DragAndDropPermissions implements android.os.Parcelable {
    DragAndDropPermissions() {}
    public void release() {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
