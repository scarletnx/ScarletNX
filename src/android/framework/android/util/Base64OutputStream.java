// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.ByteArrayOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.OutputStream;

/** Base64-encodes the bytes written to it. The encoded text goes to the stream at close. */
public class Base64OutputStream extends FilterOutputStream {
    private final int flags;
    private final ByteArrayOutputStream pending = new ByteArrayOutputStream();
    private boolean closed;

    public Base64OutputStream(OutputStream out, int flags) {
        super(out);
        this.flags = flags;
    }

    @Override public void write(int b) throws IOException { pending.write(b); }
    @Override public void write(byte[] b, int off, int len) throws IOException { pending.write(b, off, len); }

    @Override public void close() throws IOException {
        if (closed) return;
        closed = true;
        out.write(Base64.encode(pending.toByteArray(), flags));
        if ((flags & Base64.NO_CLOSE) != 0) out.flush(); else out.close();
    }
}
