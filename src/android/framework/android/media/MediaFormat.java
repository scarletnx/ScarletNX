// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/** The format of media data, as a map of keys to values. */
public final class MediaFormat {
    public static final String KEY_MIME = "mime", KEY_LANGUAGE = "language", KEY_SAMPLE_RATE = "sample-rate",
        KEY_CHANNEL_COUNT = "channel-count", KEY_WIDTH = "width", KEY_HEIGHT = "height", KEY_MAX_WIDTH = "max-width",
        KEY_MAX_HEIGHT = "max-height", KEY_MAX_INPUT_SIZE = "max-input-size", KEY_BIT_RATE = "bitrate",
        KEY_COLOR_FORMAT = "color-format", KEY_FRAME_RATE = "frame-rate", KEY_I_FRAME_INTERVAL = "i-frame-interval",
        KEY_DURATION = "durationUs", KEY_IS_ADTS = "is-adts", KEY_AAC_PROFILE = "aac-profile", KEY_PROFILE = "profile",
        KEY_LEVEL = "level", KEY_ROTATION = "rotation-degrees", KEY_PCM_ENCODING = "pcm-encoding",
        KEY_CHANNEL_MASK = "channel-mask", KEY_TRACK_ID = "track-id";
    public static final String MIMETYPE_VIDEO_AVC = "video/avc", MIMETYPE_VIDEO_HEVC = "video/hevc",
        MIMETYPE_VIDEO_VP8 = "video/x-vnd.on2.vp8", MIMETYPE_VIDEO_VP9 = "video/x-vnd.on2.vp9", MIMETYPE_VIDEO_AV1 = "video/av01",
        MIMETYPE_AUDIO_AAC = "audio/mp4a-latm", MIMETYPE_AUDIO_MPEG = "audio/mpeg", MIMETYPE_AUDIO_OPUS = "audio/opus",
        MIMETYPE_AUDIO_VORBIS = "audio/vorbis", MIMETYPE_AUDIO_RAW = "audio/raw";

    private final HashMap<String, Object> map = new HashMap<>();

    public MediaFormat() {}
    public MediaFormat(MediaFormat other) { map.putAll(other.map); }

    public static MediaFormat createAudioFormat(String mime, int sampleRate, int channelCount) {
        MediaFormat f = new MediaFormat();
        f.setString(KEY_MIME, mime);
        f.setInteger(KEY_SAMPLE_RATE, sampleRate);
        f.setInteger(KEY_CHANNEL_COUNT, channelCount);
        return f;
    }

    public static MediaFormat createVideoFormat(String mime, int width, int height) {
        MediaFormat f = new MediaFormat();
        f.setString(KEY_MIME, mime);
        f.setInteger(KEY_WIDTH, width);
        f.setInteger(KEY_HEIGHT, height);
        return f;
    }

    public boolean containsKey(String name) { return map.containsKey(name); }
    public Set<String> getKeys() { return map.keySet(); }
    public void removeKey(String name) { map.remove(name); }

    /** A missing key throws NullPointerException, and a value of another type ClassCastException, as Android documents. */
    public int getInteger(String name) { return (Integer) map.get(name); }
    public int getInteger(String name, int defaultValue) { Object v = map.get(name); return v != null ? (Integer) v : defaultValue; }
    public long getLong(String name) { return (Long) map.get(name); }
    public long getLong(String name, long defaultValue) { Object v = map.get(name); return v != null ? (Long) v : defaultValue; }
    public float getFloat(String name) { return (Float) map.get(name); }
    public float getFloat(String name, float defaultValue) { Object v = map.get(name); return v != null ? (Float) v : defaultValue; }
    public String getString(String name) { return (String) map.get(name); }
    public String getString(String name, String defaultValue) { Object v = map.get(name); return v != null ? (String) v : defaultValue; }
    public ByteBuffer getByteBuffer(String name) { return (ByteBuffer) map.get(name); }

    public void setInteger(String name, int value) { map.put(name, value); }
    public void setLong(String name, long value) { map.put(name, value); }
    public void setFloat(String name, float value) { map.put(name, value); }
    public void setString(String name, String value) { map.put(name, value); }
    public void setByteBuffer(String name, ByteBuffer bytes) { map.put(name, bytes); }

    @Override public String toString() { return map.toString(); }
}
