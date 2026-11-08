// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public interface HttpResponse extends HttpMessage {
    StatusLine getStatusLine();
    void setStatusLine(StatusLine statusline);
    void setStatusLine(ProtocolVersion ver, int code);
    void setStatusLine(ProtocolVersion ver, int code, String reason);
    void setStatusCode(int code) throws IllegalStateException;
    void setReasonPhrase(String reason) throws IllegalStateException;
    HttpEntity getEntity();
    void setEntity(HttpEntity entity);
    java.util.Locale getLocale();
    void setLocale(java.util.Locale loc);
}
