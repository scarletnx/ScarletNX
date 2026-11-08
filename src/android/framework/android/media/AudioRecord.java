// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import java.nio.ByteBuffer;

/** Audio capture on a device with no microphone: a recorder never initializes. */
public class AudioRecord {
    public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1;
    public static final int RECORDSTATE_STOPPED = 1, RECORDSTATE_RECORDING = 3;
    public static final int SUCCESS = 0, ERROR = -1, ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3, ERROR_DEAD_OBJECT = -6;
    public static final int READ_BLOCKING = 0, READ_NON_BLOCKING = 1;

    private final int sampleRate, channelConfig, audioFormat, audioSource;

    public AudioRecord(int audioSource, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes) {
        if (sampleRateInHz < 4000 || sampleRateInHz > 192000) throw new IllegalArgumentException(sampleRateInHz + "Hz is not a supported sample rate.");
        if (bufferSizeInBytes <= 0) throw new IllegalArgumentException("Invalid audio buffer size " + bufferSizeInBytes);
        this.audioSource = audioSource;
        sampleRate = sampleRateInHz;
        this.channelConfig = channelConfig;
        this.audioFormat = audioFormat;
    }

    /** The device has no input to query. */
    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) { return ERROR; }

    public int getState() { return STATE_UNINITIALIZED; }
    public int getRecordingState() { return RECORDSTATE_STOPPED; }
    public int getSampleRate() { return sampleRate; }
    public int getAudioSource() { return audioSource; }
    public int getAudioFormat() { return audioFormat; }
    public int getChannelConfiguration() { return channelConfig; }
    public int getAudioSessionId() { return 0; }

    public void startRecording() { throw new IllegalStateException("startRecording() called on an uninitialized AudioRecord."); }
    public void stop() { throw new IllegalStateException("stop() called on an uninitialized AudioRecord."); }
    public void release() {}

    public int read(byte[] audioData, int offsetInBytes, int sizeInBytes) { return ERROR_INVALID_OPERATION; }
    public int read(short[] audioData, int offsetInShorts, int sizeInShorts) { return ERROR_INVALID_OPERATION; }
    public int read(ByteBuffer audioBuffer, int sizeInBytes) { return ERROR_INVALID_OPERATION; }
}
