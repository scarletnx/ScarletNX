// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** An audio device: here the host's output, as a built-in speaker. */
public final class AudioDeviceInfo {
    public static final int TYPE_UNKNOWN = 0, TYPE_BUILTIN_EARPIECE = 1, TYPE_BUILTIN_SPEAKER = 2, TYPE_WIRED_HEADSET = 3,
        TYPE_WIRED_HEADPHONES = 4, TYPE_LINE_ANALOG = 5, TYPE_LINE_DIGITAL = 6, TYPE_BLUETOOTH_SCO = 7,
        TYPE_BLUETOOTH_A2DP = 8, TYPE_HDMI = 9, TYPE_HDMI_ARC = 10, TYPE_USB_DEVICE = 11, TYPE_USB_ACCESSORY = 12,
        TYPE_DOCK = 13, TYPE_FM = 14, TYPE_BUILTIN_MIC = 15, TYPE_FM_TUNER = 16, TYPE_TV_TUNER = 17, TYPE_TELEPHONY = 18,
        TYPE_AUX_LINE = 19, TYPE_IP = 20, TYPE_BUS = 21, TYPE_USB_HEADSET = 22, TYPE_HEARING_AID = 23,
        TYPE_BUILTIN_SPEAKER_SAFE = 24, TYPE_REMOTE_SUBMIX = 25, TYPE_BLE_HEADSET = 26, TYPE_BLE_SPEAKER = 27,
        TYPE_HDMI_EARC = 29, TYPE_BLE_BROADCAST = 30;

    private final int id, type;
    private final boolean sink;

    AudioDeviceInfo(int id, int type, boolean sink) { this.id = id; this.type = type; this.sink = sink; }

    public int getId() { return id; }
    public int getType() { return type; }
    public boolean isSink() { return sink; }
    public boolean isSource() { return !sink; }
    public CharSequence getProductName() { return "ScarletNX"; }
    public String getAddress() { return ""; }
    public int[] getSampleRates() { return new int[] { AudioTrack.NATIVE_RATE }; }
    public int[] getChannelCounts() { return new int[] { 1, 2 }; }
    public int[] getEncodings() { return new int[] { 2, 4 }; }
}
