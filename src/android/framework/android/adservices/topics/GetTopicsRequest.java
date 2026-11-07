// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.topics;

/** A request for the topics of the user's interests. */
public final class GetTopicsRequest {
    private final String adsSdkName;
    private final boolean recordObservation;

    private GetTopicsRequest(String adsSdkName, boolean recordObservation) { this.adsSdkName = adsSdkName; this.recordObservation = recordObservation; }
    public String getAdsSdkName() { return adsSdkName; }
    public boolean shouldRecordObservation() { return recordObservation; }

    public static final class Builder {
        private String adsSdkName = "";
        private boolean recordObservation = true;
        public Builder() {}
        public Builder setAdsSdkName(String adsSdkName) { this.adsSdkName = adsSdkName; return this; }
        public Builder setShouldRecordObservation(boolean recordObservation) { this.recordObservation = recordObservation; return this; }
        public GetTopicsRequest build() { return new GetTopicsRequest(adsSdkName, recordObservation); }
    }
}
