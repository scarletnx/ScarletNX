// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public interface HttpEntity {
    boolean isRepeatable();
    boolean isChunked();
    long getContentLength();
    Header getContentType();
    Header getContentEncoding();
    InputStream getContent() throws IOException, IllegalStateException;
    void writeTo(OutputStream outstream) throws IOException;
    boolean isStreaming();
    void consumeContent() throws IOException;
}
