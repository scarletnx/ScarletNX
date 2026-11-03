// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * The location service of a device with location turned off: the standard providers exist, none is enabled, no
 * fix is known, and no update comes. The device has no GNSS chip, so GNSS callbacks are not supported.
 */
public class LocationManager {
    public static final String GPS_PROVIDER = "gps", NETWORK_PROVIDER = "network", PASSIVE_PROVIDER = "passive",
        FUSED_PROVIDER = "fused";
    public static final String KEY_LOCATION_CHANGED = "location", KEY_PROVIDER_ENABLED = "providerEnabled",
        KEY_STATUS_CHANGED = "status", KEY_FLUSH_COMPLETE = "flushComplete", KEY_LOCATIONS = "locations";
    public static final String MODE_CHANGED_ACTION = "android.location.MODE_CHANGED",
        PROVIDERS_CHANGED_ACTION = "android.location.PROVIDERS_CHANGED", EXTRA_PROVIDER_NAME = "android.location.extra.PROVIDER_NAME",
        EXTRA_PROVIDER_ENABLED = "android.location.extra.PROVIDER_ENABLED", EXTRA_LOCATION_ENABLED = "android.location.extra.LOCATION_ENABLED";

    private static final List<String> PROVIDERS = List.of(PASSIVE_PROVIDER, GPS_PROVIDER, NETWORK_PROVIDER, FUSED_PROVIDER);

    public LocationManager() {}

    public List<String> getAllProviders() { return new ArrayList<>(PROVIDERS); }
    public List<String> getProviders(boolean enabledOnly) { return enabledOnly ? new ArrayList<>() : getAllProviders(); }
    public boolean hasProvider(String provider) { return PROVIDERS.contains(provider); }

    public LocationProvider getProvider(String name) {
        if (name == null) throw new IllegalArgumentException("invalid null provider");
        return hasProvider(name) ? new LocationProvider(name) : null;
    }

    public boolean isLocationEnabled() { return false; }
    public boolean isProviderEnabled(String provider) {
        if (provider == null) throw new IllegalArgumentException("invalid null provider");
        return false;
    }

    public Location getLastKnownLocation(String provider) { return null; }

    /** No fix can come, so the consumer takes null. */
    public void getCurrentLocation(String provider, CancellationSignal cancellationSignal, Executor executor, Consumer<Location> consumer) {
        executor.execute(() -> consumer.accept(null));
    }

    /** The provider is off: the listener hears that, and no update. */
    public void requestLocationUpdates(String provider, long minTimeMs, float minDistanceM, LocationListener listener, Looper looper) {
        if (provider == null) throw new IllegalArgumentException("invalid null provider");
        if (listener == null) throw new IllegalArgumentException("invalid null listener");
        new Handler(looper != null ? looper : Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper())
            .post(() -> listener.onProviderDisabled(provider));
    }

    public void requestLocationUpdates(String provider, long minTimeMs, float minDistanceM, LocationListener listener) {
        requestLocationUpdates(provider, minTimeMs, minDistanceM, listener, null);
    }

    public void requestLocationUpdates(String provider, long minTimeMs, float minDistanceM, Executor executor, LocationListener listener) {
        requestLocationUpdates(provider, new LocationRequest.Builder(minTimeMs).setMinUpdateDistanceMeters(minDistanceM).build(), executor, listener);
    }

    public void requestLocationUpdates(String provider, LocationRequest locationRequest, Executor executor, LocationListener listener) {
        if (provider == null) throw new IllegalArgumentException("invalid null provider");
        if (listener == null) throw new IllegalArgumentException("invalid null listener");
        executor.execute(() -> listener.onProviderDisabled(provider));
    }

    public void removeUpdates(LocationListener listener) {}

    public String getGnssHardwareModelName() { return null; }
    public int getGnssYearOfHardware() { return 0; }

    public boolean registerGnssStatusCallback(GnssStatus.Callback callback, Handler handler) { return false; }
    public boolean registerGnssStatusCallback(Executor executor, GnssStatus.Callback callback) { return false; }
    public void unregisterGnssStatusCallback(GnssStatus.Callback callback) {}
    public boolean registerGnssMeasurementsCallback(GnssMeasurementsEvent.Callback callback, Handler handler) { return false; }
    public boolean registerGnssMeasurementsCallback(Executor executor, GnssMeasurementsEvent.Callback callback) { return false; }
    public void unregisterGnssMeasurementsCallback(GnssMeasurementsEvent.Callback callback) {}

    @Deprecated public boolean addGpsStatusListener(GpsStatus.Listener listener) { return false; }
    @Deprecated public void removeGpsStatusListener(GpsStatus.Listener listener) {}
    @Deprecated public GpsStatus getGpsStatus(GpsStatus status) { return status != null ? status : new GpsStatus(); }
}
