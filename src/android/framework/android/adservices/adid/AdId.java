// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adid;

/** The advertising ID of the device. */
public class AdId {
    public static final String ZERO_OUT = "00000000-0000-0000-0000-000000000000";
    private final String adId;
    private final boolean limitAdTracking;

    public AdId(String adId, boolean isLimitAdTrackingEnabled) { this.adId = adId; limitAdTracking = isLimitAdTrackingEnabled; }
    public String getAdId() { return adId; }
    public boolean isLimitAdTrackingEnabled() { return limitAdTracking; }
    @Override public boolean equals(Object o) { return o instanceof AdId a && adId.equals(a.adId) && limitAdTracking == a.limitAdTracking; }
    @Override public int hashCode() { return adId.hashCode() * 2 + (limitAdTracking ? 1 : 0); }
}
