// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2012 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.security.keystore;

import java.math.BigInteger;
import java.security.spec.AlgorithmParameterSpec;
import java.util.Date;
import javax.security.auth.x500.X500Principal;

/** The parameters of a key that the Android key store makes. */
public final class KeyGenParameterSpec implements AlgorithmParameterSpec {
    private final String alias;
    private final int purposes, keySize, authValiditySeconds;
    private final String[] blockModes, encryptionPaddings;
    private final boolean authRequired, strongBox;
    private final X500Principal subject;
    private final BigInteger serial;
    private final Date notBefore, notAfter;

    private KeyGenParameterSpec(Builder b) {
        alias = b.alias; purposes = b.purposes; keySize = b.keySize; authValiditySeconds = b.authValiditySeconds;
        blockModes = b.blockModes.clone(); encryptionPaddings = b.encryptionPaddings.clone();
        authRequired = b.authRequired; strongBox = b.strongBox;
        subject = b.subject; serial = b.serial; notBefore = b.notBefore; notAfter = b.notAfter;
    }
    public String getKeystoreAlias() { return alias; }
    public int getPurposes() { return purposes; }
    public int getKeySize() { return keySize; }
    public String[] getBlockModes() { return blockModes.clone(); }
    public String[] getEncryptionPaddings() { return encryptionPaddings.clone(); }
    public boolean isUserAuthenticationRequired() { return authRequired; }
    public int getUserAuthenticationValidityDurationSeconds() { return authValiditySeconds; }
    public boolean isStrongBoxBacked() { return strongBox; }
    public X500Principal getCertificateSubject() { return subject; }
    public BigInteger getCertificateSerialNumber() { return serial; }
    public Date getCertificateNotBefore() { return notBefore; }
    public Date getCertificateNotAfter() { return notAfter; }

    public static final class Builder {
        private final String alias;
        private final int purposes;
        private int keySize = -1, authValiditySeconds;
        private String[] blockModes = new String[0], encryptionPaddings = new String[0];
        private boolean authRequired, strongBox;
        private X500Principal subject = new X500Principal("CN=fake");
        private BigInteger serial = BigInteger.ONE;
        private Date notBefore = new Date(0), notAfter = new Date(2461449600000L);

        public Builder(String keystoreAlias, int purposes) {
            if (keystoreAlias == null) throw new NullPointerException("keystoreAlias == null");
            if (keystoreAlias.isEmpty()) throw new IllegalArgumentException("keystoreAlias must not be empty");
            alias = keystoreAlias; this.purposes = purposes;
        }
        public Builder setKeySize(int keySize) { this.keySize = keySize; return this; }
        public Builder setBlockModes(String... blockModes) { this.blockModes = blockModes != null ? blockModes.clone() : new String[0]; return this; }
        public Builder setEncryptionPaddings(String... paddings) { encryptionPaddings = paddings != null ? paddings.clone() : new String[0]; return this; }
        public Builder setUserAuthenticationRequired(boolean required) { authRequired = required; return this; }
        public Builder setUserAuthenticationValidityDurationSeconds(int seconds) { authValiditySeconds = seconds; return this; }
        public Builder setIsStrongBoxBacked(boolean isStrongBoxBacked) { strongBox = isStrongBoxBacked; return this; }
        public Builder setCertificateSubject(X500Principal subject) { this.subject = subject; return this; }
        public Builder setCertificateSerialNumber(BigInteger serialNumber) { serial = serialNumber; return this; }
        public Builder setCertificateNotBefore(Date date) { notBefore = date; return this; }
        public Builder setCertificateNotAfter(Date date) { notAfter = date; return this; }
        public KeyGenParameterSpec build() { return new KeyGenParameterSpec(this); }
    }
}
