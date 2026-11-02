// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.system;

/** POSIX constants, Linux's values. */
public final class OsConstants {
    public static final int ENOENT = 2, EINTR = 4, EIO = 5, EBADF = 9, EAGAIN = 11, ENOMEM = 12, EACCES = 13, EEXIST = 17,
        ENOTDIR = 20, EISDIR = 21, EINVAL = 22, ENOSPC = 28, ESPIPE = 29, EROFS = 30, ENOSYS = 38, ENOTEMPTY = 39;
    public static final int O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 64, O_EXCL = 128, O_TRUNC = 512, O_APPEND = 1024;
    public static final int SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;
    public static final int S_IFMT = 0170000, S_IFREG = 0100000, S_IFDIR = 0040000, S_IFLNK = 0120000, S_IRWXU = 0700,
        S_IRUSR = 0400, S_IWUSR = 0200, S_IXUSR = 0100;
    public static final int F_OK = 0, R_OK = 4, W_OK = 2, X_OK = 1;
    public static final int _SC_PAGESIZE = 39, _SC_PAGE_SIZE = 39, _SC_CLK_TCK = 2, _SC_NPROCESSORS_CONF = 83, _SC_NPROCESSORS_ONLN = 84;
    public static final int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_SHARED = 1, MAP_PRIVATE = 2;
    public static final int SIGKILL = 9, SIGTERM = 15;
    public static boolean S_ISDIR(int mode) { return (mode & S_IFMT) == S_IFDIR; }
    public static boolean S_ISREG(int mode) { return (mode & S_IFMT) == S_IFREG; }
    public static boolean S_ISLNK(int mode) { return (mode & S_IFMT) == S_IFLNK; }
    public static String errnoName(int errno) { return "E" + errno; }
}
