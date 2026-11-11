// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** Told when audio devices come and go (AudioManager.registerAudioDeviceCallback). */
public abstract class AudioDeviceCallback {
    public AudioDeviceCallback() {}
    public void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) {}
    public void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {}
}
