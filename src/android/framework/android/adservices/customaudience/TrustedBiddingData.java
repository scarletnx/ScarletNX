// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.customaudience;

import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;

/** Where a buyer's trusted bidding signals come from. */
public final class TrustedBiddingData implements Parcelable {
    private final Uri uri;
    private final List<String> keys;

    private TrustedBiddingData(Uri uri, List<String> keys) { this.uri = uri; this.keys = keys; }
    public Uri getTrustedBiddingUri() { return uri; }
    public List<String> getTrustedBiddingKeys() { return keys; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<TrustedBiddingData> CREATOR = new Parcelable.Creator<>() {
        @Override public TrustedBiddingData createFromParcel(Parcel in) { return (TrustedBiddingData) in.readValue(null); }
        @Override public TrustedBiddingData[] newArray(int size) { return new TrustedBiddingData[size]; }
    };

    public static final class Builder {
        private Uri uri;
        private List<String> keys = List.of();
        public Builder() {}
        public Builder setTrustedBiddingUri(Uri trustedBiddingUri) { uri = trustedBiddingUri; return this; }
        public Builder setTrustedBiddingKeys(List<String> trustedBiddingKeys) { keys = trustedBiddingKeys; return this; }
        public TrustedBiddingData build() { return new TrustedBiddingData(uri, List.copyOf(keys)); }
    }
}
