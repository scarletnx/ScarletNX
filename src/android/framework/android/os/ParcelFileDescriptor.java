// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.Closeable;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.RandomAccessFile;

/** An open file, as a descriptor that could go to another process. */
public class ParcelFileDescriptor implements Parcelable, Closeable {
    public static final int MODE_READ_ONLY = 0x10000000;
    public static final int MODE_WRITE_ONLY = 0x20000000;
    public static final int MODE_READ_WRITE = 0x30000000;
    public static final int MODE_CREATE = 0x08000000;
    public static final int MODE_TRUNCATE = 0x04000000;
    public static final int MODE_APPEND = 0x02000000;

    /* the open file: a RandomAccessFile that opened it, or a descriptor made of a host one (fromFd, adoptFd) */
    private final RandomAccessFile file;
    private final FileDescriptor descriptor;
    private boolean closed;
    ParcelFileDescriptor(RandomAccessFile file) {
        this.file = file;
        FileDescriptor d;
        try {
            d = file.getFD();
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        descriptor = d;
    }
    private ParcelFileDescriptor(FileDescriptor descriptor) {
        file = null;
        this.descriptor = descriptor;
    }
    public static ParcelFileDescriptor open(File f, int mode) throws FileNotFoundException {
        boolean write = (mode & MODE_WRITE_ONLY) != 0;
        if (!write && !f.exists()) throw new FileNotFoundException(f.getPath());
        if ((mode & MODE_CREATE) == 0 && !f.exists()) throw new FileNotFoundException(f.getPath());
        RandomAccessFile r = new RandomAccessFile(f, write ? "rw" : "r");
        try {
            if ((mode & MODE_TRUNCATE) != 0) r.setLength(0);
            if ((mode & MODE_APPEND) != 0) r.seek(r.length());
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        return new ParcelFileDescriptor(r);
    }
    public static int parseMode(String mode) {
        switch (mode) {
            case "r": return MODE_READ_ONLY;
            case "w": case "wt": return MODE_WRITE_ONLY | MODE_CREATE | MODE_TRUNCATE;
            case "wa": return MODE_WRITE_ONLY | MODE_CREATE | MODE_APPEND;
            case "rw": return MODE_READ_WRITE | MODE_CREATE;
            case "rwt": return MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE;
            default: throw new IllegalArgumentException("Bad mode '" + mode + "'");
        }
    }
    public FileDescriptor getFileDescriptor() { return descriptor; }
    /** the host's descriptor, which the app's native code reads as on a device */
    public int getFd() {
        if (closed) throw new IllegalStateException("Already closed");
        return snx.android.os.Fds.of(descriptor);
    }
    public long getStatSize() {
        if (file != null) {
            try {
                return file.length();
            } catch (IOException e) {
                return -1;
            }
        }
        return snx.android.os.Fds.size(snx.android.os.Fds.of(descriptor));
    }
    /** the RandomAccessFile that opened the file, null for one made of a descriptor */
    public RandomAccessFile file() { return file; }
    @Override public void close() throws IOException {
        if (closed) return;
        closed = true;
        if (file != null) file.close();
        else new java.io.FileInputStream(descriptor).close();
    }
    /**
     * The caller owns the descriptor from now on, and this object is closed: the descriptor given is a duplicate of
     * this one's, which closes, so that no Java object (nor its cleaner) closes what the caller owns
     */
    public int detachFd() {
        if (closed) throw new IllegalStateException("Already closed");
        int fd = snx.android.os.Fds.dup(getFd());
        if (fd < 0) throw new IllegalStateException("snx: cannot duplicate descriptor " + getFd() + ": errno " + -fd);
        try {
            close();
        } catch (IOException e) {
            // the duplicate is the caller's either way
        }
        return fd;
    }
    /** A duplicate of a descriptor of the host, which the caller keeps */
    public static ParcelFileDescriptor fromFd(int fd) throws IOException {
        int d = snx.android.os.Fds.dup(fd);
        if (d < 0) throw new IOException("snx: cannot duplicate descriptor " + fd + ": errno " + -d);
        return new ParcelFileDescriptor(snx.android.os.Fds.wrap(d));
    }
    /** A descriptor of the host, which this object owns from now on */
    public static ParcelFileDescriptor adoptFd(int fd) { return new ParcelFileDescriptor(snx.android.os.Fds.wrap(fd)); }
    public static ParcelFileDescriptor dup(FileDescriptor orig) throws IOException { return fromFd(snx.android.os.Fds.of(orig)); }
    public ParcelFileDescriptor dup() throws IOException { return fromFd(getFd()); }
    /** A Java socket gives no descriptor to duplicate, as a socket without one. */
    public static ParcelFileDescriptor fromDatagramSocket(java.net.DatagramSocket datagramSocket) { return null; }
    @Override public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
    @Override public void writeToParcel(Parcel out, int flags) { out.writeValue(this); }

    public static class AutoCloseInputStream extends java.io.FileInputStream {
        private final ParcelFileDescriptor pfd;
        public AutoCloseInputStream(ParcelFileDescriptor pfd) { super(pfd.getFileDescriptor()); this.pfd = pfd; }
        @Override public void close() throws IOException { try { super.close(); } finally { pfd.close(); } }
    }

    public static class AutoCloseOutputStream extends java.io.FileOutputStream {
        private final ParcelFileDescriptor pfd;
        public AutoCloseOutputStream(ParcelFileDescriptor pfd) { super(pfd.getFileDescriptor()); this.pfd = pfd; }
        @Override public void close() throws IOException { try { super.close(); } finally { pfd.close(); } }
    }

    public static final Parcelable.Creator<ParcelFileDescriptor> CREATOR = new Parcelable.Creator<>() {
        @Override public ParcelFileDescriptor createFromParcel(Parcel in) { return in.readFileDescriptor(); }
        @Override public ParcelFileDescriptor[] newArray(int size) { return new ParcelFileDescriptor[size]; }
    };
}
