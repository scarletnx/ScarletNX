// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import org.apache.http.Header;
import org.apache.http.HttpEntity;
import org.apache.http.message.BasicHeader;

public abstract class AbstractHttpEntity implements HttpEntity {
    protected Header contentType, contentEncoding;
    protected boolean chunked;

    protected AbstractHttpEntity() {}
    @Override public Header getContentType() { return contentType; }
    @Override public Header getContentEncoding() { return contentEncoding; }
    @Override public boolean isChunked() { return chunked; }
    public void setContentType(Header contentType) { this.contentType = contentType; }
    public void setContentType(String ctString) { contentType = ctString != null ? new BasicHeader("Content-Type", ctString) : null; }
    public void setContentEncoding(Header contentEncoding) { this.contentEncoding = contentEncoding; }
    public void setContentEncoding(String ceString) {
        contentEncoding = ceString != null ? new BasicHeader("Content-Encoding", ceString) : null;
    }
    public void setChunked(boolean b) { chunked = b; }
    @Override public void consumeContent() throws java.io.IOException {
        if (isStreaming()) throw new UnsupportedOperationException("streaming entity does not implement consumeContent()");
    }
}
