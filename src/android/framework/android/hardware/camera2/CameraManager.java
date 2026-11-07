// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.camera2;

/** The cameras of a device that has none. */
public final class CameraManager {
    public CameraManager() {}

    public String[] getCameraIdList() { return new String[0]; }
    public void setTorchMode(String cameraId, boolean enabled) {
        throw new IllegalArgumentException("Camera " + cameraId + " is not available");
    }
}
