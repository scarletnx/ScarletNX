// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adselection;

import android.adservices.common.AdSelectionSignals;
import android.adservices.common.AdTechIdentifier;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;
import java.util.Map;

/** The inputs of an ad selection. */
public final class AdSelectionConfig implements Parcelable {
    private final AdTechIdentifier seller;
    private final Uri decisionLogicUri, trustedScoringSignalsUri;
    private final List<AdTechIdentifier> buyers;
    private final AdSelectionSignals adSelectionSignals, sellerSignals;
    private final Map<AdTechIdentifier, AdSelectionSignals> perBuyerSignals;

    private AdSelectionConfig(Builder b) {
        seller = b.seller; decisionLogicUri = b.decisionLogicUri; trustedScoringSignalsUri = b.trustedScoringSignalsUri;
        buyers = List.copyOf(b.buyers); adSelectionSignals = b.adSelectionSignals; sellerSignals = b.sellerSignals;
        perBuyerSignals = Map.copyOf(b.perBuyerSignals);
    }
    public AdTechIdentifier getSeller() { return seller; }
    public Uri getDecisionLogicUri() { return decisionLogicUri; }
    public List<AdTechIdentifier> getCustomAudienceBuyers() { return buyers; }
    public AdSelectionSignals getAdSelectionSignals() { return adSelectionSignals; }
    public AdSelectionSignals getSellerSignals() { return sellerSignals; }
    public Map<AdTechIdentifier, AdSelectionSignals> getPerBuyerSignals() { return perBuyerSignals; }
    public Uri getTrustedScoringSignalsUri() { return trustedScoringSignalsUri; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<AdSelectionConfig> CREATOR = new Parcelable.Creator<>() {
        @Override public AdSelectionConfig createFromParcel(Parcel in) { return (AdSelectionConfig) in.readValue(null); }
        @Override public AdSelectionConfig[] newArray(int size) { return new AdSelectionConfig[size]; }
    };

    public static final class Builder {
        private AdTechIdentifier seller;
        private Uri decisionLogicUri, trustedScoringSignalsUri;
        private List<AdTechIdentifier> buyers = List.of();
        private AdSelectionSignals adSelectionSignals = AdSelectionSignals.EMPTY, sellerSignals = AdSelectionSignals.EMPTY;
        private Map<AdTechIdentifier, AdSelectionSignals> perBuyerSignals = Map.of();

        public Builder() {}
        public Builder setSeller(AdTechIdentifier seller) { this.seller = seller; return this; }
        public Builder setDecisionLogicUri(Uri uri) { decisionLogicUri = uri; return this; }
        public Builder setCustomAudienceBuyers(List<AdTechIdentifier> customAudienceBuyers) { buyers = customAudienceBuyers; return this; }
        public Builder setAdSelectionSignals(AdSelectionSignals signals) { adSelectionSignals = signals; return this; }
        public Builder setSellerSignals(AdSelectionSignals signals) { sellerSignals = signals; return this; }
        public Builder setPerBuyerSignals(Map<AdTechIdentifier, AdSelectionSignals> signals) { perBuyerSignals = signals; return this; }
        public Builder setTrustedScoringSignalsUri(Uri uri) { trustedScoringSignalsUri = uri; return this; }
        public AdSelectionConfig build() { return new AdSelectionConfig(this); }
    }
}
