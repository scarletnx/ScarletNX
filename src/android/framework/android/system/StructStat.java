// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.system;

/** A file's status. */
public final class StructStat {
    public final long st_dev, st_ino, st_nlink, st_rdev, st_size, st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
    public final int st_mode, st_uid, st_gid;
    public final StructTimespec st_atim, st_mtim, st_ctim;

    public StructStat(long st_dev, long st_ino, int st_mode, long st_nlink, int st_uid, int st_gid, long st_rdev, long st_size,
                      long st_atime, long st_mtime, long st_ctime, long st_blksize, long st_blocks) {
        this.st_dev = st_dev; this.st_ino = st_ino; this.st_mode = st_mode; this.st_nlink = st_nlink; this.st_uid = st_uid;
        this.st_gid = st_gid; this.st_rdev = st_rdev; this.st_size = st_size; this.st_atime = st_atime; this.st_mtime = st_mtime;
        this.st_ctime = st_ctime; this.st_blksize = st_blksize; this.st_blocks = st_blocks;
        st_atim = new StructTimespec(st_atime, 0); st_mtim = new StructTimespec(st_mtime, 0); st_ctim = new StructTimespec(st_ctime, 0);
    }
}
