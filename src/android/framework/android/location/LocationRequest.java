// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2012 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.location;

import android.os.Parcel;
import android.os.Parcelable;

/** How often and how well a client wants locations. */
public final class LocationRequest implements Parcelable {
    public static final long PASSIVE_INTERVAL = Long.MAX_VALUE;
    public static final int QUALITY_HIGH_ACCURACY = 100, QUALITY_BALANCED_POWER_ACCURACY = 102, QUALITY_LOW_POWER = 104;

    private final long interval, minUpdateInterval, duration, maxUpdateDelay;
    private final int quality, maxUpdates;
    private final float minUpdateDistance;

    private LocationRequest(long interval, int quality, long duration, int maxUpdates, long minUpdateInterval,
            float minUpdateDistance, long maxUpdateDelay) {
        this.interval = interval; this.quality = quality; this.duration = duration; this.maxUpdates = maxUpdates;
        this.minUpdateInterval = minUpdateInterval; this.minUpdateDistance = minUpdateDistance; this.maxUpdateDelay = maxUpdateDelay;
    }

    public long getIntervalMillis() { return interval; }
    public int getQuality() { return quality; }
    public long getDurationMillis() { return duration; }
    public int getMaxUpdates() { return maxUpdates; }
    public long getMinUpdateIntervalMillis() { return minUpdateInterval; }
    public float getMinUpdateDistanceMeters() { return minUpdateDistance; }
    public long getMaxUpdateDelayMillis() { return maxUpdateDelay; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<LocationRequest> CREATOR = new Parcelable.Creator<>() {
        @Override public LocationRequest createFromParcel(Parcel in) { return (LocationRequest) in.readValue(null); }
        @Override public LocationRequest[] newArray(int size) { return new LocationRequest[size]; }
    };

    public static final class Builder {
        private static final long IMPLICIT_MIN_UPDATE_INTERVAL = -1;

        private long interval;
        private int quality = QUALITY_BALANCED_POWER_ACCURACY;
        private long duration = Long.MAX_VALUE;
        private int maxUpdates = Integer.MAX_VALUE;
        private long minUpdateInterval = IMPLICIT_MIN_UPDATE_INTERVAL;
        private float minUpdateDistance;
        private long maxUpdateDelay;

        public Builder(long intervalMillis) { setIntervalMillis(intervalMillis); }

        public Builder(LocationRequest r) {
            interval = r.interval; quality = r.quality; duration = r.duration; maxUpdates = r.maxUpdates;
            minUpdateInterval = r.minUpdateInterval; minUpdateDistance = r.minUpdateDistance; maxUpdateDelay = r.maxUpdateDelay;
        }

        private static long positive(long v, String what) {
            if (v < 0) throw new IllegalArgumentException(what + " must be greater than or equal to 0");
            return v;
        }

        public Builder setIntervalMillis(long intervalMillis) { interval = positive(intervalMillis, "intervalMillis"); return this; }

        public Builder setQuality(int quality) {
            if (quality != QUALITY_HIGH_ACCURACY && quality != QUALITY_BALANCED_POWER_ACCURACY && quality != QUALITY_LOW_POWER) {
                throw new IllegalArgumentException("quality must be a defined QUALITY constant, not " + quality);
            }
            this.quality = quality;
            return this;
        }

        public Builder setDurationMillis(long durationMillis) {
            if (durationMillis <= 0) throw new IllegalArgumentException("durationMillis must be greater than 0");
            duration = durationMillis;
            return this;
        }

        public Builder setMaxUpdates(int maxUpdates) {
            if (maxUpdates <= 0) throw new IllegalArgumentException("maxUpdates must be greater than 0");
            this.maxUpdates = maxUpdates;
            return this;
        }

        public Builder setMinUpdateIntervalMillis(long minUpdateIntervalMillis) {
            minUpdateInterval = positive(minUpdateIntervalMillis, "minUpdateIntervalMillis");
            return this;
        }

        public Builder clearMinUpdateIntervalMillis() { minUpdateInterval = IMPLICIT_MIN_UPDATE_INTERVAL; return this; }

        public Builder setMinUpdateDistanceMeters(float minUpdateDistanceMeters) {
            if (!(minUpdateDistanceMeters >= 0)) throw new IllegalArgumentException("minUpdateDistanceMeters must be greater than or equal to 0");
            minUpdateDistance = minUpdateDistanceMeters;
            return this;
        }

        public Builder setMaxUpdateDelayMillis(long maxUpdateDelayMillis) { maxUpdateDelay = positive(maxUpdateDelayMillis, "maxUpdateDelayMillis"); return this; }

        /** The implicit minimum update interval is the interval, as Android sets it. */
        public LocationRequest build() {
            if (interval == PASSIVE_INTERVAL && minUpdateInterval == IMPLICIT_MIN_UPDATE_INTERVAL) {
                throw new IllegalStateException("passive location requests must have an explicit minimum update interval");
            }
            long min = minUpdateInterval == IMPLICIT_MIN_UPDATE_INTERVAL ? interval : Math.min(minUpdateInterval, interval);
            return new LocationRequest(interval, quality, duration, maxUpdates, min, minUpdateDistance, maxUpdateDelay);
        }
    }
}
