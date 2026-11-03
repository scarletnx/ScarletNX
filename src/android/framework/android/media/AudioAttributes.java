// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.Objects;

/** What a sound is for: its usage, its content type and its flags. */
public final class AudioAttributes implements Parcelable {
    public static final int USAGE_UNKNOWN = 0, USAGE_MEDIA = 1, USAGE_VOICE_COMMUNICATION = 2,
        USAGE_VOICE_COMMUNICATION_SIGNALLING = 3, USAGE_ALARM = 4, USAGE_NOTIFICATION = 5,
        USAGE_NOTIFICATION_RINGTONE = 6, USAGE_NOTIFICATION_COMMUNICATION_REQUEST = 7,
        USAGE_NOTIFICATION_COMMUNICATION_INSTANT = 8, USAGE_NOTIFICATION_COMMUNICATION_DELAYED = 9,
        USAGE_NOTIFICATION_EVENT = 10, USAGE_ASSISTANCE_ACCESSIBILITY = 11, USAGE_ASSISTANCE_NAVIGATION_GUIDANCE = 12,
        USAGE_ASSISTANCE_SONIFICATION = 13, USAGE_GAME = 14, USAGE_ASSISTANT = 16;
    public static final int CONTENT_TYPE_UNKNOWN = 0, CONTENT_TYPE_SPEECH = 1, CONTENT_TYPE_MUSIC = 2,
        CONTENT_TYPE_MOVIE = 3, CONTENT_TYPE_SONIFICATION = 4;
    public static final int FLAG_AUDIBILITY_ENFORCED = 1, FLAG_HW_AV_SYNC = 0x10, FLAG_LOW_LATENCY = 0x100;
    public static final int ALLOW_CAPTURE_BY_ALL = 1, ALLOW_CAPTURE_BY_SYSTEM = 2, ALLOW_CAPTURE_BY_NONE = 3;

    private final int usage, contentType, flags, legacyStream;

    private AudioAttributes(int usage, int contentType, int flags, int legacyStream) {
        this.usage = usage; this.contentType = contentType; this.flags = flags; this.legacyStream = legacyStream;
    }

    public int getUsage() { return usage; }
    public int getContentType() { return contentType; }
    public int getFlags() { return flags; }

    /** The stream whose volume controls this sound. */
    public int getVolumeControlStream() {
        if (legacyStream != AudioManager.USE_DEFAULT_STREAM_TYPE) return legacyStream;
        return switch (usage) {
            case USAGE_VOICE_COMMUNICATION -> AudioManager.STREAM_VOICE_CALL;
            case USAGE_VOICE_COMMUNICATION_SIGNALLING -> AudioManager.STREAM_DTMF;
            case USAGE_ALARM -> AudioManager.STREAM_ALARM;
            case USAGE_NOTIFICATION_RINGTONE -> AudioManager.STREAM_RING;
            case USAGE_NOTIFICATION, USAGE_NOTIFICATION_COMMUNICATION_REQUEST, USAGE_NOTIFICATION_COMMUNICATION_INSTANT,
                USAGE_NOTIFICATION_COMMUNICATION_DELAYED, USAGE_NOTIFICATION_EVENT -> AudioManager.STREAM_NOTIFICATION;
            case USAGE_ASSISTANCE_ACCESSIBILITY -> AudioManager.STREAM_ACCESSIBILITY;
            case USAGE_ASSISTANCE_SONIFICATION -> AudioManager.STREAM_SYSTEM;
            default -> AudioManager.STREAM_MUSIC;
        };
    }

    @Override public boolean equals(Object o) {
        return o instanceof AudioAttributes a && a.usage == usage && a.contentType == contentType && a.flags == flags;
    }
    @Override public int hashCode() { return Objects.hash(usage, contentType, flags); }
    @Override public String toString() { return "AudioAttributes: usage=" + usage + " content=" + contentType + " flags=0x" + Integer.toHexString(flags); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int f) { dest.writeInt(usage); dest.writeInt(contentType); dest.writeInt(flags); dest.writeInt(legacyStream); }
    public static final Parcelable.Creator<AudioAttributes> CREATOR = new Parcelable.Creator<>() {
        @Override public AudioAttributes createFromParcel(Parcel in) { return new AudioAttributes(in.readInt(), in.readInt(), in.readInt(), in.readInt()); }
        @Override public AudioAttributes[] newArray(int size) { return new AudioAttributes[size]; }
    };

    public static class Builder {
        private int usage = USAGE_UNKNOWN, contentType = CONTENT_TYPE_UNKNOWN, flags;
        private int legacyStream = AudioManager.USE_DEFAULT_STREAM_TYPE;

        public Builder() {}
        public Builder(AudioAttributes aa) { usage = aa.usage; contentType = aa.contentType; flags = aa.flags; legacyStream = aa.legacyStream; }

        public AudioAttributes build() { return new AudioAttributes(usage, contentType, flags, legacyStream); }

        public Builder setUsage(int usage) { this.usage = usage; return this; }
        public Builder setContentType(int contentType) { this.contentType = contentType; return this; }
        public Builder setFlags(int flags) { this.flags |= flags & 0x3ff; return this; }
        public Builder setAllowedCapturePolicy(int capturePolicy) { return this; }
        public Builder setHapticChannelsMuted(boolean muted) { return this; }

        /** The usage and the content type of a legacy stream type. */
        public Builder setLegacyStreamType(int streamType) {
            legacyStream = streamType;
            switch (streamType) {
                case AudioManager.STREAM_VOICE_CALL -> { contentType = CONTENT_TYPE_SPEECH; usage = USAGE_VOICE_COMMUNICATION; }
                case AudioManager.STREAM_SYSTEM -> { contentType = CONTENT_TYPE_SONIFICATION; usage = USAGE_ASSISTANCE_SONIFICATION; }
                case AudioManager.STREAM_RING -> { contentType = CONTENT_TYPE_SONIFICATION; usage = USAGE_NOTIFICATION_RINGTONE; }
                case AudioManager.STREAM_MUSIC -> { contentType = CONTENT_TYPE_MUSIC; usage = USAGE_MEDIA; }
                case AudioManager.STREAM_ALARM -> { contentType = CONTENT_TYPE_SONIFICATION; usage = USAGE_ALARM; }
                case AudioManager.STREAM_NOTIFICATION -> { contentType = CONTENT_TYPE_SONIFICATION; usage = USAGE_NOTIFICATION; }
                case AudioManager.STREAM_DTMF -> { contentType = CONTENT_TYPE_SONIFICATION; usage = USAGE_VOICE_COMMUNICATION_SIGNALLING; }
                case AudioManager.STREAM_ACCESSIBILITY -> { contentType = CONTENT_TYPE_SPEECH; usage = USAGE_ASSISTANCE_ACCESSIBILITY; }
                default -> { contentType = CONTENT_TYPE_UNKNOWN; usage = USAGE_UNKNOWN; }
            }
            return this;
        }
    }
}
