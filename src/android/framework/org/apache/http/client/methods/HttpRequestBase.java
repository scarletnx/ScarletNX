// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.methods;

import java.net.URI;
import org.apache.http.ProtocolVersion;
import org.apache.http.RequestLine;
import org.apache.http.message.AbstractHttpMessage;
import org.apache.http.message.BasicRequestLine;
import org.apache.http.params.HttpProtocolParams;

public abstract class HttpRequestBase extends AbstractHttpMessage implements HttpUriRequest, AbortableHttpRequest, Cloneable {
    private URI uri;
    private volatile boolean aborted;

    public HttpRequestBase() {}
    public abstract String getMethod();
    @Override public ProtocolVersion getProtocolVersion() { return HttpProtocolParams.getVersion(getParams()); }
    @Override public URI getURI() { return uri; }
    public void setURI(URI uri) { this.uri = uri; }
    @Override public RequestLine getRequestLine() {
        String path = uri != null ? uri.toASCIIString() : null;
        return new BasicRequestLine(getMethod(), path == null || path.isEmpty() ? "/" : path, getProtocolVersion());
    }
    /** the request is not sent again; one that runs ends when its connection does */
    @Override public void abort() { aborted = true; }
    @Override public boolean isAborted() { return aborted; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
