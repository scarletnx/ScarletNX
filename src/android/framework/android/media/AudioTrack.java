// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import android.os.SystemClock;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * PCM output. The Java side has no host audio device, so a track plays silence in real time: the play head moves
 * at the sample rate, a streaming write blocks while its buffer is full, and a static track ends at its last frame.
 */
public class AudioTrack {
    public static final int MODE_STATIC = 0, MODE_STREAM = 1;
    public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1, STATE_NO_STATIC_DATA = 2;
    public static final int PLAYSTATE_STOPPED = 1, PLAYSTATE_PAUSED = 2, PLAYSTATE_PLAYING = 3;
    public static final int SUCCESS = 0, ERROR = -1, ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3, ERROR_DEAD_OBJECT = -6;
    public static final int WRITE_BLOCKING = 0, WRITE_NON_BLOCKING = 1;
    static final int NATIVE_RATE = 48000;

    private static final AtomicInteger sessions = new AtomicInteger(1);

    private final int streamType, sampleRate, channelConfig, audioFormat, mode, frameSize, bufferFrames, sessionId;
    private int state, playState = PLAYSTATE_STOPPED;
    private long written, headBase, playStart;
    private float volume = 1f;

    static int newSessionId() { return sessions.getAndIncrement(); }

    private static int channels(int config) {
        if (config == 2) return 1;
        if (config == 3) return 2;
        int n = Integer.bitCount(config);
        return n > 0 ? n : 0;
    }

    private static int bytesPerSample(int format) {
        return switch (format) {
            case 1, 2 -> 2;
            case 3 -> 1;
            case 4 -> 4;
            default -> 0;
        };
    }

    @Deprecated
    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode) {
        this(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode, 0);
    }

    @Deprecated
    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes, int mode, int sessionId) {
        if (sampleRateInHz < 4000 || sampleRateInHz > 192000) throw new IllegalArgumentException(sampleRateInHz + "Hz is not a supported sample rate.");
        int ch = channels(channelConfig), bps = bytesPerSample(audioFormat);
        if (ch == 0) throw new IllegalArgumentException("Unsupported channel mask");
        if (bps == 0) throw new IllegalArgumentException("Unsupported audio encoding.");
        if (mode != MODE_STATIC && mode != MODE_STREAM) throw new IllegalArgumentException("Invalid mode.");
        if (bufferSizeInBytes <= 0 || bufferSizeInBytes % (ch * bps) != 0) throw new IllegalArgumentException("Invalid audio buffer size.");
        this.streamType = streamType;
        sampleRate = sampleRateInHz;
        this.channelConfig = channelConfig;
        this.audioFormat = audioFormat;
        this.mode = mode;
        frameSize = ch * bps;
        bufferFrames = bufferSizeInBytes / frameSize;
        this.sessionId = sessionId != 0 ? sessionId : newSessionId();
        state = mode == MODE_STATIC ? STATE_NO_STATIC_DATA : STATE_INITIALIZED;
    }

    /** 20 ms of frames, the least that the host mixer takes. */
    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
        int ch = channels(channelConfig), bps = bytesPerSample(audioFormat);
        if (ch == 0 || bps == 0 || sampleRateInHz < 4000 || sampleRateInHz > 192000) return ERROR_BAD_VALUE;
        return sampleRateInHz / 50 * ch * bps;
    }

    public static float getMinVolume() { return 0f; }
    public static float getMaxVolume() { return 1f; }
    public static int getNativeOutputSampleRate(int streamType) { return NATIVE_RATE; }

    public int getState() { return state; }
    public int getStreamType() { return streamType; }
    public int getSampleRate() { return sampleRate; }
    public int getPlaybackRate() { return sampleRate; }
    public int getAudioFormat() { return audioFormat; }
    public int getChannelConfiguration() { return channelConfig; }
    public int getChannelCount() { return frameSize / bytesPerSample(audioFormat); }
    public int getAudioSessionId() { return sessionId; }
    public int getBufferSizeInFrames() { return bufferFrames; }
    public int getBufferCapacityInFrames() { return bufferFrames; }
    public synchronized int getPlayState() { return playState; }

    private long head() {
        if (playState != PLAYSTATE_PLAYING) return headBase;
        long played = (SystemClock.elapsedRealtimeNanos() - playStart) * sampleRate / 1000000000L;
        return Math.min(written, headBase + played);
    }

    public synchronized int getPlaybackHeadPosition() { return (int) head(); }

    private void check() {
        if (state == STATE_UNINITIALIZED) throw new IllegalStateException("play() called on uninitialized AudioTrack.");
    }

    public synchronized void play() {
        check();
        if (playState == PLAYSTATE_PLAYING) return;
        playState = PLAYSTATE_PLAYING;
        playStart = SystemClock.elapsedRealtimeNanos();
        notifyAll();
    }

    public synchronized void pause() {
        check();
        headBase = head();
        playState = PLAYSTATE_PAUSED;
    }

    public synchronized void stop() {
        check();
        headBase = mode == MODE_STREAM ? head() : 0;
        if (mode == MODE_STREAM) written = headBase;
        playState = PLAYSTATE_STOPPED;
        notifyAll();
    }

    /** The frames written and not played yet are dropped, outside play. */
    public synchronized void flush() {
        if (mode == MODE_STREAM && playState != PLAYSTATE_PLAYING) written = headBase;
    }

    public void release() {
        synchronized (this) {
            if (state != STATE_UNINITIALIZED && playState != PLAYSTATE_STOPPED) stop();
            state = STATE_UNINITIALIZED;
            notifyAll();
        }
    }

    public int setVolume(float gain) { volume = Math.max(0f, Math.min(gain, 1f)); return SUCCESS; }
    @Deprecated public int setStereoVolume(float leftGain, float rightGain) { return setVolume(Math.max(leftGain, rightGain)); }

    /** Takes sizeInBytes of whole frames, and waits while the buffer is full and the track plays. */
    private int take(int sizeInBytes, boolean blocking) {
        if (state == STATE_UNINITIALIZED) return ERROR_INVALID_OPERATION;
        if (sizeInBytes < 0) return ERROR_BAD_VALUE;
        long frames = sizeInBytes / frameSize;
        synchronized (this) {
            if (mode == MODE_STATIC) {
                written += frames;
                state = STATE_INITIALIZED;
                return (int) (frames * frameSize);
            }
            long done = 0;
            while (done < frames && state != STATE_UNINITIALIZED) {
                long room = bufferFrames - (written - head());
                if (room > 0) {
                    long n = Math.min(room, frames - done);
                    written += n;
                    done += n;
                    continue;
                }
                if (!blocking || playState != PLAYSTATE_PLAYING) break;
                long waitMs = Math.max(1, (written - head() - bufferFrames / 2) * 1000 / sampleRate);
                try { wait(waitMs); } catch (InterruptedException e) { break; }
            }
            return (int) (done * frameSize);
        }
    }

    public int write(byte[] audioData, int offsetInBytes, int sizeInBytes) {
        if (audioData == null || offsetInBytes < 0 || sizeInBytes < 0 || offsetInBytes + sizeInBytes > audioData.length) return ERROR_BAD_VALUE;
        return take(sizeInBytes, true);
    }

    public int write(byte[] audioData, int offsetInBytes, int sizeInBytes, int writeMode) {
        if (audioData == null || offsetInBytes < 0 || sizeInBytes < 0 || offsetInBytes + sizeInBytes > audioData.length) return ERROR_BAD_VALUE;
        return take(sizeInBytes, writeMode == WRITE_BLOCKING);
    }

    public int write(short[] audioData, int offsetInShorts, int sizeInShorts) {
        if (audioData == null || offsetInShorts < 0 || sizeInShorts < 0 || offsetInShorts + sizeInShorts > audioData.length) return ERROR_BAD_VALUE;
        int r = take(sizeInShorts * 2, true);
        return r > 0 ? r / 2 : r;
    }

    public int write(float[] audioData, int offsetInFloats, int sizeInFloats, int writeMode) {
        if (audioData == null || offsetInFloats < 0 || sizeInFloats < 0 || offsetInFloats + sizeInFloats > audioData.length) return ERROR_BAD_VALUE;
        int r = take(sizeInFloats * 4, writeMode == WRITE_BLOCKING);
        return r > 0 ? r / 4 : r;
    }

    public int write(ByteBuffer audioData, int sizeInBytes, int writeMode) {
        if (audioData == null || sizeInBytes < 0 || sizeInBytes > audioData.remaining()) return ERROR_BAD_VALUE;
        int r = take(sizeInBytes, writeMode == WRITE_BLOCKING);
        if (r > 0) audioData.position(audioData.position() + r);
        return r;
    }
}
