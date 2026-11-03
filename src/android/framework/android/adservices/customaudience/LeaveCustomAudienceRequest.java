// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.customaudience;

import android.adservices.common.AdTechIdentifier;

/** A request to leave a custom audience. */
public class LeaveCustomAudienceRequest {
    private final AdTechIdentifier buyer;
    private final String name;

    private LeaveCustomAudienceRequest(AdTechIdentifier buyer, String name) { this.buyer = buyer; this.name = name; }
    public AdTechIdentifier getBuyer() { return buyer; }
    public String getName() { return name; }

    public static final class Builder {
        private AdTechIdentifier buyer;
        private String name;
        public Builder() {}
        public Builder setBuyer(AdTechIdentifier buyer) { this.buyer = buyer; return this; }
        public Builder setName(String name) { this.name = name; return this; }
        public LeaveCustomAudienceRequest build() { return new LeaveCustomAudienceRequest(buyer, name); }
    }
}
