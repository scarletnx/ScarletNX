// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.entity;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UnsupportedEncodingException;

public class StringEntity extends AbstractHttpEntity implements Cloneable {
    protected final byte[] content;

    public StringEntity(String s, String charset) throws UnsupportedEncodingException {
        if (s == null) throw new IllegalArgumentException("Source string may not be null");
        if (charset == null) charset = "ISO-8859-1";
        content = s.getBytes(charset);
        setContentType("text/plain; charset=" + charset);
    }
    public StringEntity(String s) throws UnsupportedEncodingException { this(s, null); }
    @Override public boolean isRepeatable() { return true; }
    @Override public long getContentLength() { return content.length; }
    @Override public InputStream getContent() { return new ByteArrayInputStream(content); }
    @Override public void writeTo(OutputStream outstream) throws IOException { outstream.write(content); }
    @Override public boolean isStreaming() { return false; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
