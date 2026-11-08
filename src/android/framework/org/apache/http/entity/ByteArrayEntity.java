// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class ByteArrayEntity extends AbstractHttpEntity implements Cloneable {
    protected final byte[] content;

    public ByteArrayEntity(byte[] b) {
        if (b == null) throw new IllegalArgumentException("Source byte array may not be null");
        content = b;
    }
    @Override public boolean isRepeatable() { return true; }
    @Override public long getContentLength() { return content.length; }
    @Override public InputStream getContent() { return new ByteArrayInputStream(content); }
    @Override public void writeTo(OutputStream outstream) throws IOException { outstream.write(content); }
    @Override public boolean isStreaming() { return false; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
