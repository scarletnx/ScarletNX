// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** An input event that the system signed. Here no event is verified. */
public abstract class VerifiedInputEvent implements android.os.Parcelable {
    VerifiedInputEvent() {}
    public int getDeviceId() { return -1; }
    public long getEventTimeNanos() { return 0; }
    public int getSource() { return 0; }
    public int getDisplayId() { return 0; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
