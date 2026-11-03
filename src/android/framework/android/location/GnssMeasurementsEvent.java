// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

/** One set of raw GNSS measurements. No GNSS chip makes one here. */
public final class GnssMeasurementsEvent {
    private GnssMeasurementsEvent() {}

    public abstract static class Callback {
        public static final int STATUS_NOT_SUPPORTED = 0, STATUS_READY = 1, STATUS_LOCATION_DISABLED = 2, STATUS_NOT_ALLOWED = 3;

        public Callback() {}
        public void onGnssMeasurementsReceived(GnssMeasurementsEvent eventArgs) {}
        public void onStatusChanged(int status) {}
    }
}
