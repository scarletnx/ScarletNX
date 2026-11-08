// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import org.apache.http.Header;
import org.apache.http.HttpEntity;

public class HttpEntityWrapper implements HttpEntity {
    protected HttpEntity wrappedEntity;

    public HttpEntityWrapper(HttpEntity wrapped) {
        if (wrapped == null) throw new IllegalArgumentException("wrapped entity must not be null");
        wrappedEntity = wrapped;
    }
    @Override public boolean isRepeatable() { return wrappedEntity.isRepeatable(); }
    @Override public boolean isChunked() { return wrappedEntity.isChunked(); }
    @Override public long getContentLength() { return wrappedEntity.getContentLength(); }
    @Override public Header getContentType() { return wrappedEntity.getContentType(); }
    @Override public Header getContentEncoding() { return wrappedEntity.getContentEncoding(); }
    @Override public InputStream getContent() throws IOException { return wrappedEntity.getContent(); }
    @Override public void writeTo(OutputStream outstream) throws IOException { wrappedEntity.writeTo(outstream); }
    @Override public boolean isStreaming() { return wrappedEntity.isStreaming(); }
    @Override public void consumeContent() throws IOException { wrappedEntity.consumeContent(); }
    @Override @SuppressWarnings("removal") protected void finalize() throws Throwable { super.finalize(); }
}
