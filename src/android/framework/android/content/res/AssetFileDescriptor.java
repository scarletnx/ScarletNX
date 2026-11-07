// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.os.ParcelFileDescriptor;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

/** A part of an open file: an asset, which the runtime copies out of the APK to a file first. */
public class AssetFileDescriptor implements java.io.Closeable {
    public static final long UNKNOWN_LENGTH = -1;
    private final ParcelFileDescriptor fd;
    private final long start, length;

    public AssetFileDescriptor(ParcelFileDescriptor fd, long startOffset, long length) {
        this.fd = fd;
        start = startOffset;
        this.length = length;
    }

    public ParcelFileDescriptor getParcelFileDescriptor() { return fd; }
    public FileDescriptor getFileDescriptor() { return fd.getFileDescriptor(); }
    public long getStartOffset() { return start; }
    public long getLength() { return length >= 0 ? length : fd.getStatSize(); }
    public long getDeclaredLength() { return length; }
    @Override public void close() throws IOException { fd.close(); }

    public FileInputStream createInputStream() throws IOException {
        fd.file().seek(start);
        return new ParcelFileDescriptor.AutoCloseInputStream(fd);
    }

    public FileOutputStream createOutputStream() throws IOException {
        return new ParcelFileDescriptor.AutoCloseOutputStream(fd);
    }
}
