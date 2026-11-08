// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.nfc.tech;

import android.nfc.Tag;
import java.io.IOException;

/** An NFC-A (ISO 14443-3A) tag's connection: no tag reaches the app here, so get gives null for one without it. */
public final class NfcA implements java.io.Closeable {
    private final Tag tag;
    private boolean connected;

    private NfcA(Tag tag) { this.tag = tag; }
    public static NfcA get(Tag tag) {
        if (tag == null) return null;
        for (String t : tag.getTechList()) if (NfcA.class.getName().equals(t)) return new NfcA(tag);
        return null;
    }
    public Tag getTag() { return tag; }
    public void connect() throws IOException { throw new IOException("NFC is not available"); }
    public boolean isConnected() { return connected; }
    @Override public void close() { connected = false; }
    public int getMaxTransceiveLength() { return 253; }
    public byte[] transceive(byte[] data) throws IOException { throw new IOException("Tag is not connected"); }
    public byte[] getAtqa() { return new byte[2]; }
    public short getSak() { return 0; }
    public void setTimeout(int timeout) {}
    public int getTimeout() { return 0; }
}
