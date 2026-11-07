// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

import android.os.Parcel;
import android.os.Parcelable;

/** A user account: there are none. */
public class Account implements Parcelable {
    public final String name, type;
    public Account(String name, String type) { this.name = name; this.type = type; }
    public Account(Parcel in) { this(in.readString(), in.readString()); }
    @Override public boolean equals(Object o) { return o instanceof Account a && name.equals(a.name) && type.equals(a.type); }
    @Override public int hashCode() { return 31 * name.hashCode() + type.hashCode(); }
    @Override public String toString() { return "Account {name=" + name + ", type=" + type + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeString(name); dest.writeString(type); }
    public static final Parcelable.Creator<Account> CREATOR = new Parcelable.Creator<>() {
        @Override public Account createFromParcel(Parcel in) { return new Account(in); }
        @Override public Account[] newArray(int size) { return new Account[size]; }
    };
}
