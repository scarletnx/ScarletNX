// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A package's signing details. */
public final class SigningInfo {
    private final Signature[] signers;
    public SigningInfo(Signature[] signers) { this.signers = signers; }
    public boolean hasMultipleSigners() { return false; }
    public boolean hasPastSigningCertificates() { return false; }
    public Signature[] getSigningCertificateHistory() { return signers; }
    public Signature[] getApkContentsSigners() { return signers; }
}
