// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** Media DRM: no schemes. */
public final class MediaDrm implements AutoCloseable {
    public static boolean isCryptoSchemeSupported(java.util.UUID uuid) { return false; }
    public MediaDrm(java.util.UUID uuid) throws UnsupportedSchemeException { throw new UnsupportedSchemeException("snx: no DRM"); }
    @Override public void close() {}
    public void release() {}

    public static final class UnsupportedSchemeException extends Exception {
        public UnsupportedSchemeException(String detailMessage) { super(detailMessage); }
    }
}
