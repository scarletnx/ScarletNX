// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

import android.os.Bundle;
import java.util.List;

/** Takes location updates and provider changes. */
public interface LocationListener {
    void onLocationChanged(Location location);

    default void onLocationChanged(List<Location> locations) { for (Location l : locations) onLocationChanged(l); }
    default void onFlushComplete(int requestCode) {}
    @Deprecated default void onStatusChanged(String provider, int status, Bundle extras) {}
    default void onProviderEnabled(String provider) {}
    default void onProviderDisabled(String provider) {}
}
