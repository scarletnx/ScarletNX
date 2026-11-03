// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.customaudience;

/** A request to join a custom audience. */
public class JoinCustomAudienceRequest {
    private final CustomAudience audience;

    private JoinCustomAudienceRequest(CustomAudience audience) { this.audience = audience; }
    public CustomAudience getCustomAudience() { return audience; }

    public static final class Builder {
        private CustomAudience audience;
        public Builder() {}
        public Builder setCustomAudience(CustomAudience customAudience) { audience = customAudience; return this; }
        public JoinCustomAudienceRequest build() { return new JoinCustomAudienceRequest(audience); }
    }
}
