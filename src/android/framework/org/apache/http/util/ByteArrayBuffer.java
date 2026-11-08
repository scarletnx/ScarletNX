// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.util;

public final class ByteArrayBuffer {
    private byte[] buffer;
    private int len;

    public ByteArrayBuffer(int capacity) {
        if (capacity < 0) throw new IllegalArgumentException("Buffer capacity may not be negative");
        buffer = new byte[capacity];
    }
    private void expand(int newlen) {
        if (newlen > buffer.length) buffer = java.util.Arrays.copyOf(buffer, Math.max(buffer.length << 1, newlen));
    }
    public void append(byte[] b, int off, int length) {
        if (b == null) return;
        if (off < 0 || length < 0 || off + length > b.length) throw new IndexOutOfBoundsException();
        expand(len + length);
        System.arraycopy(b, off, buffer, len, length);
        len += length;
    }
    public void append(int b) {
        expand(len + 1);
        buffer[len++] = (byte) b;
    }
    public void clear() { len = 0; }
    public byte[] toByteArray() { return java.util.Arrays.copyOf(buffer, len); }
    public int byteAt(int i) { return buffer[i]; }
    public int capacity() { return buffer.length; }
    public int length() { return len; }
    public byte[] buffer() { return buffer; }
    public void setLength(int len) { this.len = len; }
    public boolean isEmpty() { return len == 0; }
    public boolean isFull() { return len == buffer.length; }
}
