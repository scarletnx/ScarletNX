// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** The old network info: connected. */
@Deprecated
public class NetworkInfo implements android.os.Parcelable {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }
    public boolean isConnected() { return true; }
    public boolean isConnectedOrConnecting() { return true; }
    public boolean isAvailable() { return true; }
    public boolean isRoaming() { return false; }
    public int getType() { return ConnectivityManager.TYPE_ETHERNET; }
    public String getTypeName() { return "ETHERNET"; }
    public State getState() { return State.CONNECTED; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
