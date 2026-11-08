// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adselection;

/** A request to report the impression of a selected ad. */
public class ReportImpressionRequest {
    private final long adSelectionId;
    private final AdSelectionConfig config;

    public ReportImpressionRequest(long adSelectionId, AdSelectionConfig adSelectionConfig) {
        this.adSelectionId = adSelectionId; config = adSelectionConfig;
    }
    public long getAdSelectionId() { return adSelectionId; }
    public AdSelectionConfig getAdSelectionConfig() { return config; }
}
