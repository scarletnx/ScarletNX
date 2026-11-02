// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** The lock screen: never locked. */
public class KeyguardManager {
    public boolean isKeyguardLocked() { return false; }
    public boolean isKeyguardSecure() { return false; }
    public boolean isDeviceLocked() { return false; }
    public boolean isDeviceSecure() { return false; }
}
