// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

import org.apache.http.HttpVersion;
import org.apache.http.ProtocolVersion;

public final class HttpProtocolParams implements CoreProtocolPNames {
    private HttpProtocolParams() {}
    public static String getHttpElementCharset(HttpParams params) {
        return params.getParameter(HTTP_ELEMENT_CHARSET) instanceof String s ? s : "US-ASCII";
    }
    public static void setHttpElementCharset(HttpParams params, String charset) { params.setParameter(HTTP_ELEMENT_CHARSET, charset); }
    public static String getContentCharset(HttpParams params) {
        return params.getParameter(HTTP_CONTENT_CHARSET) instanceof String s ? s : "ISO-8859-1";
    }
    public static void setContentCharset(HttpParams params, String charset) { params.setParameter(HTTP_CONTENT_CHARSET, charset); }
    public static ProtocolVersion getVersion(HttpParams params) {
        return params.getParameter(PROTOCOL_VERSION) instanceof ProtocolVersion v ? v : HttpVersion.HTTP_1_1;
    }
    public static void setVersion(HttpParams params, ProtocolVersion version) { params.setParameter(PROTOCOL_VERSION, version); }
    public static String getUserAgent(HttpParams params) { return (String) params.getParameter(USER_AGENT); }
    public static void setUserAgent(HttpParams params, String useragent) { params.setParameter(USER_AGENT, useragent); }
    public static boolean useExpectContinue(HttpParams params) { return params.getBooleanParameter(USE_EXPECT_CONTINUE, false); }
    public static void setUseExpectContinue(HttpParams params, boolean b) { params.setBooleanParameter(USE_EXPECT_CONTINUE, b); }
}
