// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.location;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** A geographic location: a fix from a provider, with what the provider measured. */
public class Location implements Parcelable {
    public static final int FORMAT_DEGREES = 0, FORMAT_MINUTES = 1, FORMAT_SECONDS = 2;

    private static final int HAS_ALTITUDE = 1, HAS_SPEED = 2, HAS_BEARING = 4, HAS_ACCURACY = 8,
        HAS_VERTICAL_ACCURACY = 16, HAS_SPEED_ACCURACY = 32, HAS_BEARING_ACCURACY = 64, HAS_MOCK = 128;

    private String provider;
    private long time, elapsedRealtimeNanos;
    private double latitude, longitude, altitude;
    private float speed, bearing, accuracy, verticalAccuracy, speedAccuracy, bearingAccuracy;
    private int fields;
    private Bundle extras;

    public Location(String provider) { this.provider = provider; }
    public Location(Location l) { set(l); }

    public void set(Location l) {
        provider = l.provider; time = l.time; elapsedRealtimeNanos = l.elapsedRealtimeNanos;
        latitude = l.latitude; longitude = l.longitude; altitude = l.altitude;
        speed = l.speed; bearing = l.bearing; accuracy = l.accuracy; verticalAccuracy = l.verticalAccuracy;
        speedAccuracy = l.speedAccuracy; bearingAccuracy = l.bearingAccuracy; fields = l.fields;
        extras = l.extras != null ? new Bundle(l.extras) : null;
    }

    public void reset() { set(new Location((String) null)); }

    private boolean has(int f) { return (fields & f) != 0; }
    private void put(int f, boolean on) { fields = on ? fields | f : fields & ~f; }

    public String getProvider() { return provider; }
    public void setProvider(String provider) { this.provider = provider; }
    public long getTime() { return time; }
    public void setTime(long time) { this.time = time; }
    public long getElapsedRealtimeNanos() { return elapsedRealtimeNanos; }
    public void setElapsedRealtimeNanos(long nanos) { elapsedRealtimeNanos = nanos; }
    public long getElapsedRealtimeMillis() { return elapsedRealtimeNanos / 1000000; }
    public double getLatitude() { return latitude; }
    public void setLatitude(double latitude) { this.latitude = latitude; }
    public double getLongitude() { return longitude; }
    public void setLongitude(double longitude) { this.longitude = longitude; }

    public boolean hasAltitude() { return has(HAS_ALTITUDE); }
    public double getAltitude() { return altitude; }
    public void setAltitude(double altitude) { this.altitude = altitude; put(HAS_ALTITUDE, true); }
    public void removeAltitude() { altitude = 0; put(HAS_ALTITUDE, false); }
    public boolean hasSpeed() { return has(HAS_SPEED); }
    public float getSpeed() { return speed; }
    public void setSpeed(float speed) { this.speed = speed; put(HAS_SPEED, true); }
    public void removeSpeed() { speed = 0; put(HAS_SPEED, false); }
    public boolean hasBearing() { return has(HAS_BEARING); }
    public float getBearing() { return bearing; }
    public void setBearing(float bearing) {
        float b = bearing % 360f;
        this.bearing = b < 0 ? b + 360f : b;
        put(HAS_BEARING, true);
    }
    public void removeBearing() { bearing = 0; put(HAS_BEARING, false); }
    public boolean hasAccuracy() { return has(HAS_ACCURACY); }
    public float getAccuracy() { return accuracy; }
    public void setAccuracy(float accuracy) { this.accuracy = accuracy; put(HAS_ACCURACY, true); }
    public void removeAccuracy() { accuracy = 0; put(HAS_ACCURACY, false); }
    public boolean hasVerticalAccuracy() { return has(HAS_VERTICAL_ACCURACY); }
    public float getVerticalAccuracyMeters() { return verticalAccuracy; }
    public void setVerticalAccuracyMeters(float meters) { verticalAccuracy = meters; put(HAS_VERTICAL_ACCURACY, true); }
    public boolean hasSpeedAccuracy() { return has(HAS_SPEED_ACCURACY); }
    public float getSpeedAccuracyMetersPerSecond() { return speedAccuracy; }
    public void setSpeedAccuracyMetersPerSecond(float value) { speedAccuracy = value; put(HAS_SPEED_ACCURACY, true); }
    public boolean hasBearingAccuracy() { return has(HAS_BEARING_ACCURACY); }
    public float getBearingAccuracyDegrees() { return bearingAccuracy; }
    public void setBearingAccuracyDegrees(float degrees) { bearingAccuracy = degrees; put(HAS_BEARING_ACCURACY, true); }

    @Deprecated public boolean isFromMockProvider() { return has(HAS_MOCK); }
    public boolean isMock() { return has(HAS_MOCK); }
    public void setMock(boolean mock) { put(HAS_MOCK, mock); }

    public Bundle getExtras() { return extras; }
    public void setExtras(Bundle extras) { this.extras = extras != null ? new Bundle(extras) : null; }

    @Override public String toString() {
        return "Location[" + provider + " " + latitude + "," + longitude + (hasAccuracy() ? " hAcc=" + accuracy : "") + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(provider); dest.writeInt(fields); dest.writeLong(time); dest.writeLong(elapsedRealtimeNanos);
        dest.writeDouble(latitude); dest.writeDouble(longitude); dest.writeDouble(altitude);
        dest.writeFloat(speed); dest.writeFloat(bearing); dest.writeFloat(accuracy); dest.writeFloat(verticalAccuracy);
        dest.writeFloat(speedAccuracy); dest.writeFloat(bearingAccuracy); dest.writeBundle(extras);
    }
    public static final Parcelable.Creator<Location> CREATOR = new Parcelable.Creator<>() {
        @Override public Location createFromParcel(Parcel in) {
            Location l = new Location(in.readString());
            l.fields = in.readInt(); l.time = in.readLong(); l.elapsedRealtimeNanos = in.readLong();
            l.latitude = in.readDouble(); l.longitude = in.readDouble(); l.altitude = in.readDouble();
            l.speed = in.readFloat(); l.bearing = in.readFloat(); l.accuracy = in.readFloat(); l.verticalAccuracy = in.readFloat();
            l.speedAccuracy = in.readFloat(); l.bearingAccuracy = in.readFloat(); l.extras = in.readBundle();
            return l;
        }
        @Override public Location[] newArray(int size) { return new Location[size]; }
    };
}
