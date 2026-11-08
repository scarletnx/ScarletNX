// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.ssl;

import java.io.IOException;
import java.security.cert.X509Certificate;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;

/** A verifier by a certificate's names: the JDK's HTTPS check where it has one, else the names' match. */
public abstract class AbstractVerifier implements X509HostnameVerifier {
    public AbstractVerifier() {}
    @Override public final void verify(String host, SSLSocket ssl) throws IOException {
        if (host == null) throw new NullPointerException("host to verify is null");
        SSLSession session = ssl.getSession();
        java.security.cert.Certificate[] certs = session.getPeerCertificates();
        verify(host, (X509Certificate) certs[0]);
    }
    @Override public final boolean verify(String host, SSLSession session) {
        try {
            verify(host, (X509Certificate) session.getPeerCertificates()[0]);
            return true;
        } catch (SSLException e) {
            return false;
        }
    }
    @Override public final void verify(String host, X509Certificate cert) throws SSLException {
        java.util.ArrayList<String> alts = new java.util.ArrayList<>();
        try {
            if (cert.getSubjectAlternativeNames() != null)
                for (java.util.List<?> alt : cert.getSubjectAlternativeNames())
                    if (alt.size() >= 2 && alt.get(1) instanceof String s) alts.add(s);
        } catch (java.security.cert.CertificateParsingException e) {
            // no alternative names
        }
        String cn = null;
        for (String part : cert.getSubjectX500Principal().getName().split(","))
            if (part.trim().startsWith("CN=")) cn = part.trim().substring(3);
        verify(host, cn != null ? new String[] {cn} : null, alts.toArray(new String[0]));
    }
    public final void verify(String host, String[] cns, String[] subjectAlts, boolean strictWithSubDomains) throws SSLException {
        java.util.ArrayList<String> names = new java.util.ArrayList<>();
        if (cns != null && cns.length > 0 && cns[0] != null) names.add(cns[0]);
        if (subjectAlts != null) java.util.Collections.addAll(names, subjectAlts);
        String h = host.trim().toLowerCase(java.util.Locale.ENGLISH);
        for (String name : names) {
            String n = name.toLowerCase(java.util.Locale.ENGLISH);
            if (n.equals(h)) return;
            if (n.startsWith("*.") && h.endsWith(n.substring(1))
                && (!strictWithSubDomains || h.indexOf('.') == h.length() - n.length() + 1))
                return;
        }
        throw new SSLException("hostname in certificate didn't match: <" + host + "> != " + names);
    }
}
