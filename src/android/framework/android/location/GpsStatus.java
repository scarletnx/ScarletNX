// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

import java.util.ArrayList;

/** The old GPS status: no satellite is seen here. */
@Deprecated
public final class GpsStatus {
    public static final int GPS_EVENT_STARTED = 1, GPS_EVENT_STOPPED = 2, GPS_EVENT_FIRST_FIX = 3, GPS_EVENT_SATELLITE_STATUS = 4;

    private final ArrayList<GpsSatellite> satellites = new ArrayList<>();
    private int timeToFirstFix;

    GpsStatus() {}

    public int getTimeToFirstFix() { return timeToFirstFix; }
    public Iterable<GpsSatellite> getSatellites() { return new ArrayList<>(satellites); }
    public int getMaxSatellites() { return 255; }

    public interface Listener { void onGpsStatusChanged(int event); }
    public interface NmeaListener { void onNmeaReceived(long timestamp, String nmea); }
}
