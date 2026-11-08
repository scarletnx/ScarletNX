// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.nfc;

/** The NFC service (Off in docs/android/INTERFACES.md). The device has no NFC adapter. */
public final class NfcManager {
    private NfcManager() {}

    /** the device's adapter: none */
    public NfcAdapter getDefaultAdapter() { return null; }
}
