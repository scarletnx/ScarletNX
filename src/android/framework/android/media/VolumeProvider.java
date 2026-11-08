// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** The volume of a remote playback device, which a media session controls. */
public abstract class VolumeProvider {
    public static final int VOLUME_CONTROL_FIXED = 0, VOLUME_CONTROL_RELATIVE = 1, VOLUME_CONTROL_ABSOLUTE = 2;

    private final int control, maxVolume;
    private final String controlId;
    private int currentVolume;

    public VolumeProvider(int volumeControl, int maxVolume, int currentVolume) { this(volumeControl, maxVolume, currentVolume, null); }

    public VolumeProvider(int volumeControl, int maxVolume, int currentVolume, String volumeControlId) {
        control = volumeControl;
        this.maxVolume = maxVolume;
        this.currentVolume = currentVolume;
        controlId = volumeControlId;
    }

    public final int getVolumeControl() { return control; }
    public final int getMaxVolume() { return maxVolume; }
    public final int getCurrentVolume() { return currentVolume; }
    public final String getVolumeControlId() { return controlId; }

    /** The session's controllers read the volume when they ask for it. */
    public final void setCurrentVolume(int currentVolume) { this.currentVolume = currentVolume; }

    public void onSetVolumeTo(int volume) {}
    public void onAdjustVolume(int direction) {}
}
