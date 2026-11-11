// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import android.os.Parcel;
import android.os.Parcelable;

/** An audio format: its encoding, sample rate and channel masks. A value not set is 0 (none). */
public final class AudioFormat implements Parcelable {
    public static final int ENCODING_INVALID = 0, ENCODING_DEFAULT = 1, ENCODING_PCM_16BIT = 2, ENCODING_PCM_8BIT = 3,
        ENCODING_PCM_FLOAT = 4, ENCODING_AC3 = 5, ENCODING_E_AC3 = 6, ENCODING_DTS = 7, ENCODING_DTS_HD = 8,
        ENCODING_MP3 = 9, ENCODING_AAC_LC = 10, ENCODING_AAC_HE_V1 = 11, ENCODING_AAC_HE_V2 = 12, ENCODING_IEC61937 = 13,
        ENCODING_DOLBY_TRUEHD = 14, ENCODING_AAC_ELD = 15, ENCODING_AAC_XHE = 16, ENCODING_AC4 = 17,
        ENCODING_E_AC3_JOC = 18, ENCODING_DOLBY_MAT = 19, ENCODING_OPUS = 20, ENCODING_PCM_24BIT_PACKED = 21,
        ENCODING_PCM_32BIT = 22;
    public static final int SAMPLE_RATE_UNSPECIFIED = 0;
    public static final int CHANNEL_INVALID = 0, CHANNEL_OUT_DEFAULT = 1, CHANNEL_OUT_FRONT_LEFT = 0x4,
        CHANNEL_OUT_FRONT_RIGHT = 0x8, CHANNEL_OUT_FRONT_CENTER = 0x10, CHANNEL_OUT_LOW_FREQUENCY = 0x20,
        CHANNEL_OUT_BACK_LEFT = 0x40, CHANNEL_OUT_BACK_RIGHT = 0x80, CHANNEL_OUT_MONO = CHANNEL_OUT_FRONT_LEFT,
        CHANNEL_OUT_STEREO = CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT,
        CHANNEL_OUT_QUAD = CHANNEL_OUT_STEREO | CHANNEL_OUT_BACK_LEFT | CHANNEL_OUT_BACK_RIGHT,
        CHANNEL_OUT_5POINT1 = CHANNEL_OUT_QUAD | CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_LOW_FREQUENCY;
    public static final int CHANNEL_IN_DEFAULT = 1, CHANNEL_IN_LEFT = 0x4, CHANNEL_IN_RIGHT = 0x8,
        CHANNEL_IN_FRONT = 0x10, CHANNEL_IN_BACK = 0x20, CHANNEL_IN_MONO = CHANNEL_IN_FRONT,
        CHANNEL_IN_STEREO = CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT;

    private final int encoding, sampleRate, channelMask, channelIndexMask;

    private AudioFormat(int encoding, int sampleRate, int channelMask, int channelIndexMask) {
        this.encoding = encoding;
        this.sampleRate = sampleRate;
        this.channelMask = channelMask;
        this.channelIndexMask = channelIndexMask;
    }

    public int getEncoding() { return encoding; }
    public int getSampleRate() { return sampleRate; }
    public int getChannelMask() { return channelMask; }
    public int getChannelIndexMask() { return channelIndexMask; }
    public int getChannelCount() {
        int n = Integer.bitCount(channelIndexMask);
        return n != 0 ? n : Integer.bitCount(channelMask);
    }
    /** the bytes of one frame of linear PCM, 1 for another encoding */
    public int getFrameSizeInBytes() {
        int sample = switch (encoding) {
            case ENCODING_PCM_8BIT -> 1;
            case ENCODING_PCM_16BIT, ENCODING_DEFAULT -> 2;
            case ENCODING_PCM_24BIT_PACKED -> 3;
            case ENCODING_PCM_FLOAT, ENCODING_PCM_32BIT -> 4;
            default -> 0;
        };
        return sample != 0 ? sample * Math.max(1, getChannelCount()) : 1;
    }

    @Override public boolean equals(Object o) {
        return o instanceof AudioFormat f && f.encoding == encoding && f.sampleRate == sampleRate
            && f.channelMask == channelMask && f.channelIndexMask == channelIndexMask;
    }
    @Override public int hashCode() { return java.util.Objects.hash(encoding, sampleRate, channelMask, channelIndexMask); }
    @Override public String toString() {
        return "AudioFormat: props=0, enc=" + encoding + ", chan=0x" + Integer.toHexString(channelMask) + ", chan_index=0x"
            + Integer.toHexString(channelIndexMask) + ", rate=" + sampleRate;
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(encoding);
        dest.writeInt(sampleRate);
        dest.writeInt(channelMask);
        dest.writeInt(channelIndexMask);
    }
    public static final Parcelable.Creator<AudioFormat> CREATOR = new Parcelable.Creator<>() {
        @Override public AudioFormat createFromParcel(Parcel in) {
            return new AudioFormat(in.readInt(), in.readInt(), in.readInt(), in.readInt());
        }
        @Override public AudioFormat[] newArray(int size) { return new AudioFormat[size]; }
    };

    public static class Builder {
        private int encoding = ENCODING_INVALID, sampleRate = SAMPLE_RATE_UNSPECIFIED, channelMask = CHANNEL_INVALID,
            channelIndexMask;

        public Builder() {}
        public Builder(AudioFormat af) {
            encoding = af.encoding;
            sampleRate = af.sampleRate;
            channelMask = af.channelMask;
            channelIndexMask = af.channelIndexMask;
        }
        public Builder setEncoding(int encoding) {
            if (encoding == ENCODING_DEFAULT) encoding = ENCODING_PCM_16BIT;
            if (encoding < ENCODING_PCM_16BIT || encoding > ENCODING_PCM_32BIT)
                throw new IllegalArgumentException("Invalid encoding " + encoding);
            this.encoding = encoding;
            return this;
        }
        public Builder setSampleRate(int sampleRate) {
            if ((sampleRate < 4000 || sampleRate > 192000) && sampleRate != SAMPLE_RATE_UNSPECIFIED)
                throw new IllegalArgumentException("Invalid sample rate " + sampleRate);
            this.sampleRate = sampleRate;
            return this;
        }
        public Builder setChannelMask(int channelMask) {
            if (channelMask == CHANNEL_INVALID) throw new IllegalArgumentException("Invalid zero channel mask");
            this.channelMask = channelMask;
            return this;
        }
        public Builder setChannelIndexMask(int channelIndexMask) {
            if (channelIndexMask == 0) throw new IllegalArgumentException("Invalid zero channel index mask");
            this.channelIndexMask = channelIndexMask;
            return this;
        }
        public AudioFormat build() { return new AudioFormat(encoding, sampleRate, channelMask, channelIndexMask); }
    }
}
