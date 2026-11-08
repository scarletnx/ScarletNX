// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.nfc;

/**
 * An NFC adapter (Off in docs/android/INTERFACES.md): the device has none, so getDefaultAdapter gives null, as on a
 * device without NFC hardware, and no instance exists.
 */
public final class NfcAdapter {
    public static final String ACTION_NDEF_DISCOVERED = "android.nfc.action.NDEF_DISCOVERED",
        ACTION_TECH_DISCOVERED = "android.nfc.action.TECH_DISCOVERED", ACTION_TAG_DISCOVERED = "android.nfc.action.TAG_DISCOVERED",
        ACTION_ADAPTER_STATE_CHANGED = "android.nfc.action.ADAPTER_STATE_CHANGED";
    public static final String EXTRA_TAG = "android.nfc.extra.TAG", EXTRA_NDEF_MESSAGES = "android.nfc.extra.NDEF_MESSAGES",
        EXTRA_ID = "android.nfc.extra.ID", EXTRA_ADAPTER_STATE = "android.nfc.extra.ADAPTER_STATE";
    public static final int STATE_OFF = 1, STATE_TURNING_ON = 2, STATE_ON = 3, STATE_TURNING_OFF = 4;
    public static final int FLAG_READER_NFC_A = 0x1, FLAG_READER_NFC_B = 0x2, FLAG_READER_NFC_F = 0x4,
        FLAG_READER_NFC_V = 0x8, FLAG_READER_NFC_BARCODE = 0x10, FLAG_READER_SKIP_NDEF_CHECK = 0x80,
        FLAG_READER_NO_PLATFORM_SOUNDS = 0x100;

    public interface ReaderCallback { void onTagDiscovered(Tag tag); }
    public interface OnTagRemovedListener { void onTagRemoved(); }

    private NfcAdapter() {}

    public static NfcAdapter getDefaultAdapter(android.content.Context context) { return null; }
    public boolean isEnabled() { return false; }
    public void enableForegroundDispatch(android.app.Activity activity, android.app.PendingIntent intent,
                                        android.content.IntentFilter[] filters, String[][] techLists) {}
    public void disableForegroundDispatch(android.app.Activity activity) {}
    public void enableReaderMode(android.app.Activity activity, ReaderCallback callback, int flags, android.os.Bundle extras) {}
    public void disableReaderMode(android.app.Activity activity) {}
    public boolean ignore(Tag tag, int debounceMs, OnTagRemovedListener tagRemovedListener, android.os.Handler handler) { return false; }
}
