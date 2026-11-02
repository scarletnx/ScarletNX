// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A file system's space: the host's, for the folder (statvfs of the path on Android). 4 KiB blocks. */
public class StatFs {
    private static final long BLOCK = 4096;
    private java.io.File file;

    public StatFs(String path) { restat(path); }
    public void restat(String path) {
        java.io.File f = new java.io.File(path);
        if (!f.exists()) throw new IllegalArgumentException("Invalid path: " + path);
        file = f;
    }
    public long getBlockSizeLong() { return BLOCK; }
    public long getBlockCountLong() { return file.getTotalSpace() / BLOCK; }
    public long getFreeBlocksLong() { return file.getFreeSpace() / BLOCK; }
    public long getAvailableBlocksLong() { return file.getUsableSpace() / BLOCK; }
    public long getFreeBytes() { return file.getFreeSpace(); }
    public long getAvailableBytes() { return file.getUsableSpace(); }
    public long getTotalBytes() { return file.getTotalSpace(); }
    @Deprecated public int getBlockSize() { return (int) BLOCK; }
    @Deprecated public int getBlockCount() { return (int) Math.min(Integer.MAX_VALUE, getBlockCountLong()); }
    @Deprecated public int getFreeBlocks() { return (int) Math.min(Integer.MAX_VALUE, getFreeBlocksLong()); }
    @Deprecated public int getAvailableBlocks() { return (int) Math.min(Integer.MAX_VALUE, getAvailableBlocksLong()); }
}
