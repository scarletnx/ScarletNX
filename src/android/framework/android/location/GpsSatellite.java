// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

/** One satellite of a GpsStatus. */
@Deprecated
public final class GpsSatellite {
    boolean valid, hasEphemeris, hasAlmanac, usedInFix;
    int prn;
    float snr, elevation, azimuth;

    GpsSatellite(int prn) { this.prn = prn; }

    public int getPrn() { return prn; }
    public float getSnr() { return snr; }
    public float getElevation() { return elevation; }
    public float getAzimuth() { return azimuth; }
    public boolean hasEphemeris() { return hasEphemeris; }
    public boolean hasAlmanac() { return hasAlmanac; }
    public boolean usedInFix() { return usedInFix; }
}
