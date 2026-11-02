// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A Handler that another component sends messages to. */
public final class Messenger implements Parcelable {
    private final Handler target;

    public Messenger(Handler target) { this.target = target; }
    public void send(Message message) { target.sendMessage(message); }
    public IBinder getBinder() { return new Binder(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeValue(this); }
    public static final Parcelable.Creator<Messenger> CREATOR = new Parcelable.Creator<>() {
        @Override public Messenger createFromParcel(Parcel in) { return (Messenger) in.readValue(null); }
        @Override public Messenger[] newArray(int size) { return new Messenger[size]; }
    };
}
