// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.hardware.fingerprint;

import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import java.security.Signature;
import javax.crypto.Cipher;
import javax.crypto.Mac;

/** Fingerprints on a device with no fingerprint sensor. */
@Deprecated
public class FingerprintManager {
    public static final int FINGERPRINT_ERROR_HW_UNAVAILABLE = 1, FINGERPRINT_ERROR_UNABLE_TO_PROCESS = 2,
        FINGERPRINT_ERROR_TIMEOUT = 3, FINGERPRINT_ERROR_NO_SPACE = 4, FINGERPRINT_ERROR_CANCELED = 5,
        FINGERPRINT_ERROR_LOCKOUT = 7, FINGERPRINT_ERROR_VENDOR = 8, FINGERPRINT_ERROR_LOCKOUT_PERMANENT = 9,
        FINGERPRINT_ERROR_USER_CANCELED = 10, FINGERPRINT_ERROR_NO_FINGERPRINTS = 11, FINGERPRINT_ERROR_HW_NOT_PRESENT = 12;
    public static final int FINGERPRINT_ACQUIRED_GOOD = 0, FINGERPRINT_ACQUIRED_PARTIAL = 1, FINGERPRINT_ACQUIRED_INSUFFICIENT = 2,
        FINGERPRINT_ACQUIRED_IMAGER_DIRTY = 3, FINGERPRINT_ACQUIRED_TOO_SLOW = 4, FINGERPRINT_ACQUIRED_TOO_FAST = 5;

    public FingerprintManager() {}

    public boolean isHardwareDetected() { return false; }
    public boolean hasEnrolledFingerprints() { return false; }

    /** No sensor: the callback hears that the hardware is not available. */
    public void authenticate(CryptoObject crypto, CancellationSignal cancel, int flags, AuthenticationCallback callback, Handler handler) {
        if (callback == null) throw new IllegalArgumentException("Must supply an authentication callback");
        Handler h = handler != null ? handler : new Handler(Looper.getMainLooper());
        h.post(() -> callback.onAuthenticationError(FINGERPRINT_ERROR_HW_UNAVAILABLE, "Fingerprint hardware not available."));
    }

    public static final class CryptoObject {
        private final Object crypto;
        public CryptoObject(Signature signature) { crypto = signature; }
        public CryptoObject(Cipher cipher) { crypto = cipher; }
        public CryptoObject(Mac mac) { crypto = mac; }
        public Signature getSignature() { return crypto instanceof Signature s ? s : null; }
        public Cipher getCipher() { return crypto instanceof Cipher c ? c : null; }
        public Mac getMac() { return crypto instanceof Mac m ? m : null; }
    }

    public static class AuthenticationResult {
        private final CryptoObject crypto;
        AuthenticationResult(CryptoObject crypto) { this.crypto = crypto; }
        public CryptoObject getCryptoObject() { return crypto; }
    }

    public abstract static class AuthenticationCallback {
        public AuthenticationCallback() {}
        public void onAuthenticationError(int errorCode, CharSequence errString) {}
        public void onAuthenticationHelp(int helpCode, CharSequence helpString) {}
        public void onAuthenticationSucceeded(AuthenticationResult result) {}
        public void onAuthenticationFailed() {}
    }
}
