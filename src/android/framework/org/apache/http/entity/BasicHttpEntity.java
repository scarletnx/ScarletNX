// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class BasicHttpEntity extends AbstractHttpEntity {
    private InputStream content;
    private boolean contentObtained;
    private long length = -1;

    public BasicHttpEntity() {}
    @Override public long getContentLength() { return length; }
    @Override public InputStream getContent() throws IllegalStateException {
        if (content == null) throw new IllegalStateException("Content has not been provided");
        if (contentObtained) throw new IllegalStateException("Content has been consumed");
        contentObtained = true;
        return content;
    }
    @Override public boolean isRepeatable() { return false; }
    public void setContentLength(long len) { length = len; }
    public void setContent(InputStream instream) {
        content = instream;
        contentObtained = false;
    }
    @Override public void writeTo(OutputStream outstream) throws IOException {
        try (InputStream in = getContent()) { in.transferTo(outstream); }
    }
    @Override public boolean isStreaming() { return !contentObtained && content != null; }
    @Override public void consumeContent() throws IOException {
        if (content != null) content.close();
    }
}
