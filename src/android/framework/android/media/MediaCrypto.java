// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import java.util.UUID;

/** A decryption session for a codec. No DRM scheme is supported here. */
public final class MediaCrypto {
    private MediaCrypto() {}

    public static boolean isCryptoSchemeSupported(UUID uuid) { return false; }
    public boolean requiresSecureDecoderComponent(String mime) { return false; }
    public void setMediaDrmSession(byte[] sessionId) {}
    public void release() {}
}
