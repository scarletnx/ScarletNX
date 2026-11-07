// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.credentials;

/** What a later credential retrieval can give. Here no provider has a result. */
public final class PrepareGetCredentialResponse {
    /** The handle that completes a prepared retrieval. */
    public static final class PendingGetCredentialHandle {
        PendingGetCredentialHandle() {}
    }

    private final PendingGetCredentialHandle handle = new PendingGetCredentialHandle();

    PrepareGetCredentialResponse() {}
    public boolean hasCredentialResults(String credentialType) { return false; }
    public boolean hasAuthenticationResults() { return false; }
    public boolean hasRemoteResults() { return false; }
    public PendingGetCredentialHandle getPendingGetCredentialHandle() { return handle; }
}
