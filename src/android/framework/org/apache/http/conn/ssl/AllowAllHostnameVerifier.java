// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.ssl;

public class AllowAllHostnameVerifier extends AbstractVerifier {
    public AllowAllHostnameVerifier() {}
    @Override public final void verify(String host, String[] cns, String[] subjectAlts) {}
    @Override public final String toString() { return "ALLOW_ALL"; }
}
