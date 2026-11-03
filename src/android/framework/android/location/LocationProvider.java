// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

/** A source of locations, by name. */
@Deprecated
public class LocationProvider {
    public static final int OUT_OF_SERVICE = 0, TEMPORARILY_UNAVAILABLE = 1, AVAILABLE = 2;

    private final String name;

    LocationProvider(String name) { this.name = name; }

    public String getName() { return name; }
    public boolean requiresNetwork() { return name.equals(LocationManager.NETWORK_PROVIDER); }
    public boolean requiresSatellite() { return name.equals(LocationManager.GPS_PROVIDER); }
    public boolean requiresCell() { return name.equals(LocationManager.NETWORK_PROVIDER); }
    public boolean hasMonetaryCost() { return false; }
    public boolean supportsAltitude() { return name.equals(LocationManager.GPS_PROVIDER); }
    public boolean supportsSpeed() { return name.equals(LocationManager.GPS_PROVIDER); }
    public boolean supportsBearing() { return name.equals(LocationManager.GPS_PROVIDER); }
    public int getPowerRequirement() { return 1; }
    public int getAccuracy() { return name.equals(LocationManager.GPS_PROVIDER) ? 1 : 2; }
}
