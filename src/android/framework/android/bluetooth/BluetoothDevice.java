// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.bluetooth;

import android.os.Parcel;
import android.os.Parcelable;

/** A remote Bluetooth device. There is no Bluetooth, so an app never gets one. */
public final class BluetoothDevice implements Parcelable {
    private BluetoothDevice() {}
    public String getName() { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<BluetoothDevice> CREATOR = new Parcelable.Creator<>() {
        @Override public BluetoothDevice createFromParcel(Parcel in) { return (BluetoothDevice) in.readValue(null); }
        @Override public BluetoothDevice[] newArray(int size) { return new BluetoothDevice[size]; }
    };
}
