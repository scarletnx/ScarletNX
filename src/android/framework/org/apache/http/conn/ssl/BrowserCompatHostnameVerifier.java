// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.ssl;

import javax.net.ssl.SSLException;

public class BrowserCompatHostnameVerifier extends AbstractVerifier {
    public BrowserCompatHostnameVerifier() {}
    @Override public final void verify(String host, String[] cns, String[] subjectAlts) throws SSLException {
        verify(host, cns, subjectAlts, false);
    }
    @Override public final String toString() { return "BROWSER_COMPATIBLE"; }
}
