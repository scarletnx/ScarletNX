// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.biometrics;

/** Biometric authentication on a device with no biometric sensor. */
public class BiometricManager {
    public static final int BIOMETRIC_SUCCESS = 0, BIOMETRIC_ERROR_HW_UNAVAILABLE = 1, BIOMETRIC_ERROR_NONE_ENROLLED = 11,
        BIOMETRIC_ERROR_NO_HARDWARE = 12, BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;

    public interface Authenticators {
        int BIOMETRIC_STRONG = 0x000F, BIOMETRIC_WEAK = 0x00FF, DEVICE_CREDENTIAL = 1 << 15;
    }

    public BiometricManager() {}

    @Deprecated public int canAuthenticate() { return BIOMETRIC_ERROR_NO_HARDWARE; }

    /** A device credential (a lock screen) is not set either. */
    public int canAuthenticate(int authenticators) {
        return (authenticators & ~Authenticators.DEVICE_CREDENTIAL) != 0 ? BIOMETRIC_ERROR_NO_HARDWARE : BIOMETRIC_ERROR_NONE_ENROLLED;
    }
}
