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

    private final RandomAccessFile file;
    private boolean detached;

    ParcelFileDescriptor(RandomAccessFile file) { this.file = file; }

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

    public FileDescriptor getFileDescriptor() {
        try { return file.getFD(); } catch (IOException e) { throw new IllegalStateException(e); }
    }

    /** No host descriptor number reaches Java: a positive placeholder. */
    public int getFd() { return 3; }

    public long getStatSize() {
        try { return file.length(); } catch (IOException e) { return -1; }
    }

    public RandomAccessFile file() { return file; }

    @Override public void close() throws IOException { if (!detached) file.close(); }
    /** The caller owns the descriptor from now on: close() leaves it open. */
    public int detachFd() {
        if (detached) throw new IllegalStateException("Already detached");
        detached = true;
        return getFd();
    }
    /** A descriptor of the host, opened again through /proc/self/fd where the host has it. */
    public static ParcelFileDescriptor fromFd(int fd) throws IOException {
        File f = new File("/proc/self/fd/" + fd);
        if (!f.exists()) throw new IOException("snx: no host descriptor " + fd);
        try {
            return new ParcelFileDescriptor(new RandomAccessFile(f, "rw"));
        } catch (FileNotFoundException e) {
            return new ParcelFileDescriptor(new RandomAccessFile(f, "r"));
        }
    }
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
