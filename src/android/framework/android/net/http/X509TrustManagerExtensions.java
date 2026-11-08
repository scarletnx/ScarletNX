// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.http;

import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.List;
import javax.net.ssl.X509ExtendedTrustManager;
import javax.net.ssl.X509TrustManager;

/** Checks a server's chain with a trust manager, and gives the chain that it trusted. */
public class X509TrustManagerExtensions {
    private final X509TrustManager tm;

    public X509TrustManagerExtensions(X509TrustManager tm) throws IllegalArgumentException {
        if (tm == null) throw new IllegalArgumentException("tm cannot be null");
        this.tm = tm;
    }

    public List<X509Certificate> checkServerTrusted(X509Certificate[] chain, String authType, String host) throws CertificateException {
        tm.checkServerTrusted(chain, authType);
        return Arrays.asList(chain);
    }

    public boolean isUserAddedCertificate(X509Certificate cert) { return false; }
    public boolean isSameTrustConfiguration(String hostname1, String hostname2) { return true; }
}
