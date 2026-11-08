// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.app.PendingIntent;
import android.content.ComponentName;

/**
 * The audio service. Output is the host's, as one built-in speaker; the device has no microphone, no wired
 * headset and no Bluetooth. Stream volumes live in the process. Each focus request is granted.
 */
public class AudioManager {
    public static final int STREAM_VOICE_CALL = 0, STREAM_SYSTEM = 1, STREAM_RING = 2, STREAM_MUSIC = 3, STREAM_ALARM = 4,
        STREAM_NOTIFICATION = 5, STREAM_DTMF = 8, STREAM_ACCESSIBILITY = 10, USE_DEFAULT_STREAM_TYPE = Integer.MIN_VALUE;
    public static final int ADJUST_RAISE = 1, ADJUST_LOWER = -1, ADJUST_SAME = 0, ADJUST_MUTE = -100, ADJUST_UNMUTE = 100,
        ADJUST_TOGGLE_MUTE = 101;
    public static final int FLAG_SHOW_UI = 1, FLAG_ALLOW_RINGER_MODES = 2, FLAG_PLAY_SOUND = 4, FLAG_REMOVE_SOUND_AND_VIBRATE = 8,
        FLAG_VIBRATE = 16;
    public static final int RINGER_MODE_SILENT = 0, RINGER_MODE_VIBRATE = 1, RINGER_MODE_NORMAL = 2;
    public static final int MODE_INVALID = -2, MODE_CURRENT = -1, MODE_NORMAL = 0, MODE_RINGTONE = 1, MODE_IN_CALL = 2,
        MODE_IN_COMMUNICATION = 3, MODE_CALL_SCREENING = 4;
    public static final int AUDIOFOCUS_NONE = 0, AUDIOFOCUS_GAIN = 1, AUDIOFOCUS_GAIN_TRANSIENT = 2,
        AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK = 3, AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE = 4, AUDIOFOCUS_LOSS = -1,
        AUDIOFOCUS_LOSS_TRANSIENT = -2, AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK = -3;
    public static final int AUDIOFOCUS_REQUEST_FAILED = 0, AUDIOFOCUS_REQUEST_GRANTED = 1, AUDIOFOCUS_REQUEST_DELAYED = 2;
    public static final int GET_DEVICES_INPUTS = 1, GET_DEVICES_OUTPUTS = 2, GET_DEVICES_ALL = 3;
    public static final int FX_KEY_CLICK = 0, FX_FOCUS_NAVIGATION_UP = 1, FX_FOCUS_NAVIGATION_DOWN = 2,
        FX_FOCUS_NAVIGATION_LEFT = 3, FX_FOCUS_NAVIGATION_RIGHT = 4, FX_KEYPRESS_STANDARD = 5, FX_KEYPRESS_SPACEBAR = 6,
        FX_KEYPRESS_DELETE = 7, FX_KEYPRESS_RETURN = 8, FX_KEYPRESS_INVALID = 9;
    public static final int ERROR = -1, ERROR_DEAD_OBJECT = -6, AUDIO_SESSION_ID_GENERATE = 0;
    public static final String ACTION_AUDIO_BECOMING_NOISY = "android.media.AUDIO_BECOMING_NOISY",
        ACTION_HEADSET_PLUG = "android.intent.action.HEADSET_PLUG",
        ACTION_SCO_AUDIO_STATE_UPDATED = "android.media.ACTION_SCO_AUDIO_STATE_UPDATED",
        RINGER_MODE_CHANGED_ACTION = "android.media.RINGER_MODE_CHANGED",
        EXTRA_SCO_AUDIO_STATE = "android.media.extra.SCO_AUDIO_STATE";
    public static final String PROPERTY_OUTPUT_SAMPLE_RATE = "android.media.property.OUTPUT_SAMPLE_RATE",
        PROPERTY_OUTPUT_FRAMES_PER_BUFFER = "android.media.property.OUTPUT_FRAMES_PER_BUFFER",
        PROPERTY_SUPPORT_MIC_NEAR_ULTRASOUND = "android.media.property.SUPPORT_MIC_NEAR_ULTRASOUND",
        PROPERTY_SUPPORT_SPEAKER_NEAR_ULTRASOUND = "android.media.property.SUPPORT_SPEAKER_NEAR_ULTRASOUND";

    public interface OnAudioFocusChangeListener { void onAudioFocusChange(int focusChange); }

    private static final int[] MAX = { 5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15 };
    private static final int[] volume = MAX.clone();
    private static final boolean[] muted = new boolean[MAX.length];
    private static int mode = MODE_NORMAL;
    private static boolean micMute, speakerphone;
    private static final AudioDeviceInfo SPEAKER = new AudioDeviceInfo(1, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER, true);

    public AudioManager() {}

    private static int stream(int streamType) {
        if (streamType < 0 || streamType >= MAX.length) throw new IllegalArgumentException("Bad stream type " + streamType);
        return streamType;
    }

    public int getStreamMaxVolume(int streamType) { return MAX[stream(streamType)]; }
    public int getStreamMinVolume(int streamType) { stream(streamType); return streamType == STREAM_VOICE_CALL ? 1 : 0; }
    public int getStreamVolume(int streamType) { synchronized (volume) { return muted[stream(streamType)] ? 0 : volume[streamType]; } }
    public boolean isStreamMute(int streamType) { synchronized (volume) { return muted[stream(streamType)]; } }

    public void setStreamVolume(int streamType, int index, int flags) {
        synchronized (volume) { volume[stream(streamType)] = Math.max(0, Math.min(index, MAX[streamType])); }
    }

    public void adjustStreamVolume(int streamType, int direction, int flags) {
        synchronized (volume) {
            int s = stream(streamType);
            switch (direction) {
                case ADJUST_RAISE, ADJUST_LOWER -> { muted[s] = false; volume[s] = Math.max(0, Math.min(volume[s] + direction, MAX[s])); }
                case ADJUST_MUTE -> muted[s] = true;
                case ADJUST_UNMUTE -> muted[s] = false;
                case ADJUST_TOGGLE_MUTE -> muted[s] = !muted[s];
                case ADJUST_SAME -> {}
                default -> throw new IllegalArgumentException("Bad direction " + direction);
            }
        }
    }

    public void adjustVolume(int direction, int flags) { adjustStreamVolume(STREAM_MUSIC, direction, flags); }
    public void adjustSuggestedStreamVolume(int direction, int suggestedStreamType, int flags) {
        adjustStreamVolume(suggestedStreamType == USE_DEFAULT_STREAM_TYPE ? STREAM_MUSIC : suggestedStreamType, direction, flags);
    }

    public int getRingerMode() { return RINGER_MODE_NORMAL; }
    public void setRingerMode(int ringerMode) {}
    public int getMode() { return mode; }
    public void setMode(int mode) { AudioManager.mode = mode; }
    public boolean isMusicActive() { return false; }
    public boolean isVolumeFixed() { return false; }

    public AudioDeviceInfo[] getDevices(int flags) {
        return (flags & GET_DEVICES_OUTPUTS) != 0 ? new AudioDeviceInfo[] { SPEAKER } : new AudioDeviceInfo[0];
    }

    public String getParameters(String keys) { return ""; }
    public void setParameters(String keyValuePairs) {}

    /** The host mixes at 48 kHz, in buffers of 256 frames. */
    public String getProperty(String key) {
        return switch (key) {
            case PROPERTY_OUTPUT_SAMPLE_RATE -> Integer.toString(AudioTrack.NATIVE_RATE);
            case PROPERTY_OUTPUT_FRAMES_PER_BUFFER -> "256";
            case PROPERTY_SUPPORT_MIC_NEAR_ULTRASOUND, PROPERTY_SUPPORT_SPEAKER_NEAR_ULTRASOUND -> "false";
            default -> null;
        };
    }

    public int generateAudioSessionId() { return AudioTrack.newSessionId(); }

    public boolean isMicrophoneMute() { return micMute; }
    public void setMicrophoneMute(boolean on) { micMute = on; }
    public boolean isSpeakerphoneOn() { return speakerphone; }
    public void setSpeakerphoneOn(boolean on) { speakerphone = on; }
    @Deprecated public boolean isWiredHeadsetOn() { return false; }
    @Deprecated public void setWiredHeadsetOn(boolean on) {}
    public boolean isBluetoothA2dpOn() { return false; }
    public boolean isBluetoothScoOn() { return false; }
    public boolean isBluetoothScoAvailableOffCall() { return false; }
    public void setBluetoothScoOn(boolean on) {}
    public void startBluetoothSco() {}
    public void stopBluetoothSco() {}

    public void playSoundEffect(int effectType) {}
    public void playSoundEffect(int effectType, float volume) {}
    public void loadSoundEffects() {}
    public void unloadSoundEffects() {}

    @Deprecated public int requestAudioFocus(OnAudioFocusChangeListener l, int streamType, int durationHint) { return AUDIOFOCUS_REQUEST_GRANTED; }
    public int abandonAudioFocus(OnAudioFocusChangeListener l) { return AUDIOFOCUS_REQUEST_GRANTED; }

    @Deprecated public void registerMediaButtonEventReceiver(ComponentName eventReceiver) {}
    @Deprecated public void registerMediaButtonEventReceiver(PendingIntent eventReceiver) {}
    @Deprecated public void unregisterMediaButtonEventReceiver(ComponentName eventReceiver) {}
    @Deprecated public void unregisterMediaButtonEventReceiver(PendingIntent eventReceiver) {}
    @Deprecated public void registerRemoteControlClient(RemoteControlClient rcClient) {}
    @Deprecated public void unregisterRemoteControlClient(RemoteControlClient rcClient) {}
}
