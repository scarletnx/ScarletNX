// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.customaudience;

import android.adservices.common.AdData;
import android.adservices.common.AdSelectionSignals;
import android.adservices.common.AdTechIdentifier;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import java.time.Instant;
import java.util.List;

/** A group of users with a common interest, which a buyer names. */
public final class CustomAudience implements Parcelable {
    private final AdTechIdentifier buyer;
    private final String name;
    private final Instant activationTime, expirationTime;
    private final Uri dailyUpdateUri, biddingLogicUri;
    private final AdSelectionSignals userBiddingSignals;
    private final TrustedBiddingData trustedBiddingData;
    private final List<AdData> ads;

    private CustomAudience(Builder b) {
        buyer = b.buyer; name = b.name; activationTime = b.activationTime; expirationTime = b.expirationTime;
        dailyUpdateUri = b.dailyUpdateUri; biddingLogicUri = b.biddingLogicUri; userBiddingSignals = b.userBiddingSignals;
        trustedBiddingData = b.trustedBiddingData; ads = List.copyOf(b.ads);
    }
    public AdTechIdentifier getBuyer() { return buyer; }
    public String getName() { return name; }
    public Instant getActivationTime() { return activationTime; }
    public Instant getExpirationTime() { return expirationTime; }
    public Uri getDailyUpdateUri() { return dailyUpdateUri; }
    public Uri getBiddingLogicUri() { return biddingLogicUri; }
    public AdSelectionSignals getUserBiddingSignals() { return userBiddingSignals; }
    public TrustedBiddingData getTrustedBiddingData() { return trustedBiddingData; }
    public List<AdData> getAds() { return ads; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<CustomAudience> CREATOR = new Parcelable.Creator<>() {
        @Override public CustomAudience createFromParcel(Parcel in) { return (CustomAudience) in.readValue(null); }
        @Override public CustomAudience[] newArray(int size) { return new CustomAudience[size]; }
    };

    public static final class Builder {
        private AdTechIdentifier buyer;
        private String name;
        private Instant activationTime, expirationTime;
        private Uri dailyUpdateUri, biddingLogicUri;
        private AdSelectionSignals userBiddingSignals;
        private TrustedBiddingData trustedBiddingData;
        private List<AdData> ads = List.of();

        public Builder() {}
        public Builder setBuyer(AdTechIdentifier buyer) { this.buyer = buyer; return this; }
        public Builder setName(String name) { this.name = name; return this; }
        public Builder setActivationTime(Instant activationTime) { this.activationTime = activationTime; return this; }
        public Builder setExpirationTime(Instant expirationTime) { this.expirationTime = expirationTime; return this; }
        public Builder setDailyUpdateUri(Uri dailyUpdateUri) { this.dailyUpdateUri = dailyUpdateUri; return this; }
        public Builder setBiddingLogicUri(Uri biddingLogicUri) { this.biddingLogicUri = biddingLogicUri; return this; }
        public Builder setUserBiddingSignals(AdSelectionSignals signals) { userBiddingSignals = signals; return this; }
        public Builder setTrustedBiddingData(TrustedBiddingData data) { trustedBiddingData = data; return this; }
        public Builder setAds(List<AdData> ads) { this.ads = ads != null ? ads : List.of(); return this; }
        public CustomAudience build() { return new CustomAudience(this); }
    }
}
