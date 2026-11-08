// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import java.util.Locale;
import org.apache.http.HttpEntity;
import org.apache.http.HttpResponse;
import org.apache.http.ProtocolVersion;
import org.apache.http.StatusLine;

public class BasicHttpResponse extends AbstractHttpMessage implements HttpResponse {
    private StatusLine statusline;
    private HttpEntity entity;
    private Locale locale = Locale.getDefault();

    public BasicHttpResponse(StatusLine statusline) {
        if (statusline == null) throw new IllegalArgumentException("Status line may not be null.");
        this.statusline = statusline;
    }
    public BasicHttpResponse(ProtocolVersion ver, int code, String reason) { this(new BasicStatusLine(ver, code, reason)); }

    @Override public ProtocolVersion getProtocolVersion() { return statusline.getProtocolVersion(); }
    @Override public StatusLine getStatusLine() { return statusline; }
    @Override public void setStatusLine(StatusLine statusline) {
        if (statusline == null) throw new IllegalArgumentException("Status line may not be null");
        this.statusline = statusline;
    }
    @Override public void setStatusLine(ProtocolVersion ver, int code) { statusline = new BasicStatusLine(ver, code, null); }
    @Override public void setStatusLine(ProtocolVersion ver, int code, String reason) { statusline = new BasicStatusLine(ver, code, reason); }
    @Override public void setStatusCode(int code) { statusline = new BasicStatusLine(statusline.getProtocolVersion(), code, null); }
    @Override public void setReasonPhrase(String reason) {
        statusline = new BasicStatusLine(statusline.getProtocolVersion(), statusline.getStatusCode(), reason);
    }
    @Override public HttpEntity getEntity() { return entity; }
    @Override public void setEntity(HttpEntity entity) { this.entity = entity; }
    @Override public Locale getLocale() { return locale; }
    @Override public void setLocale(Locale loc) { locale = loc; }
}
