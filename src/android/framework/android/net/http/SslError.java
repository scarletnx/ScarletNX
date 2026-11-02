// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.http;

/** A TLS error of a page's load */
public class SslError {
    public static final int SSL_NOTYETVALID = 0, SSL_EXPIRED = 1, SSL_IDMISMATCH = 2, SSL_UNTRUSTED = 3,
        SSL_DATE_INVALID = 4, SSL_INVALID = 5;
    private final int error;
    private final String url;

    public SslError(int error, java.security.cert.X509Certificate certificate, String url) {
        this.error = error;
        this.url = url;
    }
    public int getPrimaryError() { return error; }
    public String getUrl() { return url; }
    public boolean hasError(int error) { return this.error == error; }
}
