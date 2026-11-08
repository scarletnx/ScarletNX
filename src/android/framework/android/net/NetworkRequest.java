// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** What network a request wants. */
public class NetworkRequest implements android.os.Parcelable {
    public static class Builder {
        public Builder() {}
        public Builder addCapability(int capability) { return this; }
        public Builder removeCapability(int capability) { return this; }
        public Builder addTransportType(int transportType) { return this; }
        public Builder removeTransportType(int transportType) { return this; }
        public Builder clearCapabilities() { return this; }
        public Builder setIncludeOtherUidNetworks(boolean include) { return this; }
        public NetworkRequest build() { return new NetworkRequest(); }
    }
    public boolean hasCapability(int capability) { return true; }
    public boolean hasTransport(int transportType) { return false; }
    public int[] getCapabilities() { return new int[0]; }
    public int[] getTransportTypes() { return new int[0]; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
