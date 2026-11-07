// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** A response that a WebViewClient gives for a request */
public class WebResourceResponse {
    private String mimeType, encoding, reason;
    private int status = 200;
    private java.util.Map<String, String> headers;
    private java.io.InputStream data;

    public WebResourceResponse(String mimeType, String encoding, java.io.InputStream data) {
        this.mimeType = mimeType;
        this.encoding = encoding;
        this.data = data;
    }
    public WebResourceResponse(String mimeType, String encoding, int statusCode, String reasonPhrase,
                               java.util.Map<String, String> responseHeaders, java.io.InputStream data) {
        this(mimeType, encoding, data);
        status = statusCode;
        reason = reasonPhrase;
        headers = responseHeaders;
    }
    public void setMimeType(String mimeType) { this.mimeType = mimeType; }
    public String getMimeType() { return mimeType; }
    public void setEncoding(String encoding) { this.encoding = encoding; }
    public String getEncoding() { return encoding; }
    public void setStatusCodeAndReasonPhrase(int statusCode, String reasonPhrase) { status = statusCode; reason = reasonPhrase; }
    public int getStatusCode() { return status; }
    public String getReasonPhrase() { return reason; }
    public void setResponseHeaders(java.util.Map<String, String> headers) { this.headers = headers; }
    public java.util.Map<String, String> getResponseHeaders() { return headers; }
    public void setData(java.io.InputStream data) { this.data = data; }
    public java.io.InputStream getData() { return data; }
}
