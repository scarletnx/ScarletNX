// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A user of the device: there is one, user 0. */
public final class UserHandle implements Parcelable {
    public static final UserHandle OWNER = new UserHandle(0);
    public static final UserHandle SYSTEM = OWNER;
    public static final UserHandle ALL = new UserHandle(-1);
    public static final UserHandle CURRENT = new UserHandle(-2);
    public static final int PER_USER_RANGE = 100000;
    private final int handle;

    public UserHandle(int h) { handle = h; }
    public static UserHandle of(int userId) { return userId == 0 ? OWNER : new UserHandle(userId); }
    public static UserHandle getUserHandleForUid(int uid) { return of(uid / PER_USER_RANGE); }
    public static int getUserId(int uid) { return uid / PER_USER_RANGE; }
    public static int myUserId() { return 0; }
    public int getIdentifier() { return handle; }
    public boolean isSystem() { return handle == 0; }
    @Override public boolean equals(Object o) { return o instanceof UserHandle u && u.handle == handle; }
    @Override public int hashCode() { return handle; }
    @Override public String toString() { return "UserHandle{" + handle + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeInt(handle); }
    public static final Parcelable.Creator<UserHandle> CREATOR = new Parcelable.Creator<>() {
        @Override public UserHandle createFromParcel(Parcel in) { return of(in.readInt()); }
        @Override public UserHandle[] newArray(int size) { return new UserHandle[size]; }
    };
}
