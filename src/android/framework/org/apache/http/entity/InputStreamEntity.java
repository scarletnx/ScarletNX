// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class InputStreamEntity extends AbstractHttpEntity {
    private final InputStream content;
    private final long length;
    private boolean consumed;

    public InputStreamEntity(InputStream instream, long length) {
        if (instream == null) throw new IllegalArgumentException("Source input stream may not be null");
        content = instream;
        this.length = length;
    }
    @Override public boolean isRepeatable() { return false; }
    @Override public long getContentLength() { return length; }
    @Override public InputStream getContent() { return content; }
    @Override public void writeTo(OutputStream outstream) throws IOException {
        byte[] buffer = new byte[4096];
        long remaining = length;
        int n;
        while ((length < 0 || remaining > 0)
               && (n = content.read(buffer, 0, length < 0 ? buffer.length : (int) Math.min(buffer.length, remaining))) != -1) {
            outstream.write(buffer, 0, n);
            remaining -= n;
        }
        consumed = true;
    }
    @Override public boolean isStreaming() { return !consumed; }
    @Override public void consumeContent() throws IOException {
        consumed = true;
        content.close();
    }
}
