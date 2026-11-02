// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.system;

import java.io.File;
import java.io.FileDescriptor;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.Paths;

/** System calls, on the JDK's file API. */
public final class Os {
    private Os() {}

    public static StructStat stat(String path) throws ErrnoException { return stat(Paths.get(path), true); }
    public static StructStat lstat(String path) throws ErrnoException { return stat(Paths.get(path), false); }
    public static StructStat fstat(FileDescriptor fd) throws ErrnoException { throw new ErrnoException("fstat", OsConstants.ENOSYS); }
    private static StructStat stat(Path p, boolean follow) throws ErrnoException {
        LinkOption[] o = follow ? new LinkOption[0] : new LinkOption[] {LinkOption.NOFOLLOW_LINKS};
        if (!Files.exists(p, o)) throw new ErrnoException("stat", OsConstants.ENOENT);
        try {
            long size = Files.isDirectory(p, o) ? 4096 : Files.size(p);
            long mtime = Files.getLastModifiedTime(p, o).toMillis() / 1000;
            int mode = (Files.isSymbolicLink(p) && !follow ? OsConstants.S_IFLNK : Files.isDirectory(p, o) ? OsConstants.S_IFDIR : OsConstants.S_IFREG) | 0600;
            return new StructStat(0, 0, mode, 1, android.os.Process.myUid(), android.os.Process.myUid(), 0, size, mtime, mtime, mtime, 4096, (size + 511) / 512);
        } catch (java.io.IOException e) {
            throw new ErrnoException("stat", OsConstants.EIO, e);
        }
    }
    public static void remove(String path) throws ErrnoException { if (!new File(path).delete()) throw new ErrnoException("remove", OsConstants.ENOENT); }
    public static void rename(String oldPath, String newPath) throws ErrnoException { if (!new File(oldPath).renameTo(new File(newPath))) throw new ErrnoException("rename", OsConstants.EIO); }
    public static void mkdir(String path, int mode) throws ErrnoException { if (!new File(path).mkdir()) throw new ErrnoException("mkdir", OsConstants.EEXIST); }
    public static void symlink(String oldPath, String newPath) throws ErrnoException {
        try { Files.createSymbolicLink(Paths.get(newPath), Paths.get(oldPath)); } catch (java.io.IOException e) { throw new ErrnoException("symlink", OsConstants.EIO, e); }
    }
    public static String readlink(String path) throws ErrnoException {
        try { return Files.readSymbolicLink(Paths.get(path)).toString(); } catch (java.io.IOException e) { throw new ErrnoException("readlink", OsConstants.EINVAL, e); }
    }
    public static boolean access(String path, int mode) throws ErrnoException {
        File f = new File(path);
        if (!f.exists()) throw new ErrnoException("access", OsConstants.ENOENT);
        return true;
    }
    public static void chmod(String path, int mode) {}
    public static long sysconf(int name) {
        if (name == OsConstants._SC_PAGESIZE) return 4096;
        if (name == OsConstants._SC_CLK_TCK) return 100;
        if (name == OsConstants._SC_NPROCESSORS_CONF || name == OsConstants._SC_NPROCESSORS_ONLN) return Runtime.getRuntime().availableProcessors();
        return -1;
    }
    public static int getpid() { return android.os.Process.myPid(); }
    public static int gettid() { return android.os.Process.myTid(); }
    public static int getuid() { return android.os.Process.myUid(); }
    public static String getenv(String name) { return System.getenv(name); }
    public static void setenv(String name, String value, boolean overwrite) {}
    public static void fsync(FileDescriptor fd) throws ErrnoException {
        try { fd.sync(); } catch (java.io.SyncFailedException e) { throw new ErrnoException("fsync", OsConstants.EIO, e); }
    }
    public static void close(FileDescriptor fd) {}
}
