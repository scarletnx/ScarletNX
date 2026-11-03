// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

import java.util.Arrays;

/** The satellites that a GNSS chip sees, by index. */
public final class GnssStatus {
    public static final int CONSTELLATION_UNKNOWN = 0, CONSTELLATION_GPS = 1, CONSTELLATION_SBAS = 2,
        CONSTELLATION_GLONASS = 3, CONSTELLATION_QZSS = 4, CONSTELLATION_BEIDOU = 5, CONSTELLATION_GALILEO = 6,
        CONSTELLATION_IRNSS = 7;

    private static final int HAS_EPHEMERIS = 1, HAS_ALMANAC = 2, USED_IN_FIX = 4, HAS_CARRIER = 8, HAS_BASEBAND = 16;

    private final int count;
    private final int[] svidAndFlags;
    private final float[] cn0, elevations, azimuths, carrierFrequencies, basebandCn0;

    private GnssStatus(int count, int[] svidAndFlags, float[] cn0, float[] elevations, float[] azimuths,
            float[] carrierFrequencies, float[] basebandCn0) {
        this.count = count;
        this.svidAndFlags = svidAndFlags;
        this.cn0 = cn0;
        this.elevations = elevations;
        this.azimuths = azimuths;
        this.carrierFrequencies = carrierFrequencies;
        this.basebandCn0 = basebandCn0;
    }

    public int getSatelliteCount() { return count; }
    public int getConstellationType(int satelliteIndex) { return (svidAndFlags[satelliteIndex] >> 8) & 0xf; }
    public int getSvid(int satelliteIndex) { return svidAndFlags[satelliteIndex] >>> 12; }
    public float getCn0DbHz(int satelliteIndex) { return cn0[satelliteIndex]; }
    public float getElevationDegrees(int satelliteIndex) { return elevations[satelliteIndex]; }
    public float getAzimuthDegrees(int satelliteIndex) { return azimuths[satelliteIndex]; }
    public boolean hasEphemerisData(int satelliteIndex) { return (svidAndFlags[satelliteIndex] & HAS_EPHEMERIS) != 0; }
    public boolean hasAlmanacData(int satelliteIndex) { return (svidAndFlags[satelliteIndex] & HAS_ALMANAC) != 0; }
    public boolean usedInFix(int satelliteIndex) { return (svidAndFlags[satelliteIndex] & USED_IN_FIX) != 0; }
    public boolean hasCarrierFrequencyHz(int satelliteIndex) { return (svidAndFlags[satelliteIndex] & HAS_CARRIER) != 0; }
    public float getCarrierFrequencyHz(int satelliteIndex) { return carrierFrequencies[satelliteIndex]; }
    public boolean hasBasebandCn0DbHz(int satelliteIndex) { return (svidAndFlags[satelliteIndex] & HAS_BASEBAND) != 0; }
    public float getBasebandCn0DbHz(int satelliteIndex) { return basebandCn0[satelliteIndex]; }

    @Override public boolean equals(Object o) {
        return o instanceof GnssStatus g && g.count == count && Arrays.equals(g.svidAndFlags, svidAndFlags)
            && Arrays.equals(g.cn0, cn0) && Arrays.equals(g.elevations, elevations) && Arrays.equals(g.azimuths, azimuths)
            && Arrays.equals(g.carrierFrequencies, carrierFrequencies) && Arrays.equals(g.basebandCn0, basebandCn0);
    }

    @Override public int hashCode() {
        return 31 * Arrays.hashCode(svidAndFlags) + Arrays.hashCode(cn0) + 7 * count;
    }

    public static final class Builder {
        private int[] svidAndFlags = new int[0];
        private float[] cn0 = new float[0], elevations = new float[0], azimuths = new float[0],
            carrier = new float[0], baseband = new float[0];

        public Builder() {}

        public Builder addSatellite(int constellationType, int svid, float cn0DbHz, float elevation, float azimuth,
                boolean hasEphemeris, boolean hasAlmanac, boolean usedInFix, boolean hasCarrierFrequency,
                float carrierFrequency, boolean hasBasebandCn0DbHz, float basebandCn0DbHz) {
            int n = svidAndFlags.length;
            svidAndFlags = Arrays.copyOf(svidAndFlags, n + 1);
            svidAndFlags[n] = (svid << 12) | ((constellationType & 0xf) << 8) | (hasEphemeris ? HAS_EPHEMERIS : 0)
                | (hasAlmanac ? HAS_ALMANAC : 0) | (usedInFix ? USED_IN_FIX : 0) | (hasCarrierFrequency ? HAS_CARRIER : 0)
                | (hasBasebandCn0DbHz ? HAS_BASEBAND : 0);
            cn0 = append(cn0, cn0DbHz);
            elevations = append(elevations, elevation);
            azimuths = append(azimuths, azimuth);
            carrier = append(carrier, carrierFrequency);
            baseband = append(baseband, basebandCn0DbHz);
            return this;
        }

        private static float[] append(float[] a, float v) { float[] b = Arrays.copyOf(a, a.length + 1); b[a.length] = v; return b; }

        public Builder clearSatellites() {
            svidAndFlags = new int[0];
            cn0 = elevations = azimuths = carrier = baseband = new float[0];
            return this;
        }

        public GnssStatus build() { return new GnssStatus(svidAndFlags.length, svidAndFlags, cn0, elevations, azimuths, carrier, baseband); }
    }

    public abstract static class Callback {
        public Callback() {}
        public void onStarted() {}
        public void onStopped() {}
        public void onFirstFix(int ttffMillis) {}
        public void onSatelliteStatusChanged(GnssStatus status) {}
    }
}
