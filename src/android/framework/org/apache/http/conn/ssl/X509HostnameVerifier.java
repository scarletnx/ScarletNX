// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.ssl;

import java.io.IOException;
import java.security.cert.X509Certificate;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLSocket;

public interface X509HostnameVerifier extends HostnameVerifier {
    void verify(String host, SSLSocket ssl) throws IOException;
    void verify(String host, X509Certificate cert) throws SSLException;
    void verify(String host, String[] cns, String[] subjectAlts) throws SSLException;
}
