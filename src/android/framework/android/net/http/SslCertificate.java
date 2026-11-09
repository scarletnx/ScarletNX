// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.http;

import java.security.cert.X509Certificate;
import java.util.Date;

/** A page's SSL certificate: whom it is issued to and by, and when it is valid */
public class SslCertificate {
    public class DName {
        private final String dn, cn, o, u;
        public DName(String dName) {
            dn = dName != null ? dName : "";
            String c = "", org = "", unit = "";
            for (String part : dn.split(",")) {
                String[] kv = part.trim().split("=", 2);
                if (kv.length != 2) continue;
                switch (kv[0].trim().toUpperCase()) {
                    case "CN" -> c = kv[1].trim();
                    case "O" -> org = kv[1].trim();
                    case "OU" -> unit = kv[1].trim();
                    default -> {}
                }
            }
            cn = c;
            o = org;
            u = unit;
        }
        public String getDName() { return dn; }
        public String getCName() { return cn; }
        public String getOName() { return o; }
        public String getUName() { return u; }
    }

    private final DName issuedTo, issuedBy;
    private final Date notBefore, notAfter;
    private final X509Certificate certificate;

    public SslCertificate(X509Certificate certificate) {
        this.certificate = certificate;
        issuedTo = new DName(certificate.getSubjectX500Principal().getName());
        issuedBy = new DName(certificate.getIssuerX500Principal().getName());
        notBefore = certificate.getNotBefore();
        notAfter = certificate.getNotAfter();
    }
    @Deprecated public SslCertificate(String issuedTo, String issuedBy, String validNotBefore, String validNotAfter) {
        this.certificate = null;
        this.issuedTo = new DName(issuedTo);
        this.issuedBy = new DName(issuedBy);
        notBefore = null;
        notAfter = null;
    }
    public Date getValidNotBeforeDate() { return notBefore; }
    public Date getValidNotAfterDate() { return notAfter; }
    public DName getIssuedTo() { return issuedTo; }
    public DName getIssuedBy() { return issuedBy; }
    public X509Certificate getX509Certificate() { return certificate; }
    @Override public String toString() { return "Issued to: " + issuedTo.getDName() + ";\nIssued by: " + issuedBy.getDName() + ";\n"; }
}
