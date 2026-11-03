// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * A file that changes all at once: a write goes to "<name>.new", and finishWrite renames it over the base file.
 * A reader sees the old file or the new one, never part of a write. A "<name>.bak" from the older scheme becomes
 * the base file again.
 */
public class AtomicFile {
    private final File baseName, newName, legacyBackupName;

    public AtomicFile(File baseName) {
        this.baseName = baseName;
        newName = new File(baseName.getPath() + ".new");
        legacyBackupName = new File(baseName.getPath() + ".bak");
    }

    public File getBaseFile() { return baseName; }

    public void delete() {
        baseName.delete();
        newName.delete();
        legacyBackupName.delete();
    }

    public boolean exists() { return baseName.exists() || legacyBackupName.exists(); }

    public FileOutputStream startWrite() throws IOException {
        if (legacyBackupName.exists()) legacyBackupName.renameTo(baseName);
        try {
            return new FileOutputStream(newName);
        } catch (FileNotFoundException e) {
            File parent = newName.getParentFile();
            if (parent == null || !parent.mkdirs()) throw new IOException("Failed to create directory for " + newName);
            return new FileOutputStream(newName);
        }
    }

    public void finishWrite(FileOutputStream str) {
        if (str == null) return;
        try {
            str.getFD().sync();
        } catch (IOException e) {
        }
        try { str.close(); } catch (IOException e) {}
        if (!newName.renameTo(baseName)) {
            baseName.delete();
            newName.renameTo(baseName);
        }
    }

    public void failWrite(FileOutputStream str) {
        if (str == null) return;
        try { str.close(); } catch (IOException e) {}
        newName.delete();
    }

    public FileInputStream openRead() throws FileNotFoundException {
        if (legacyBackupName.exists()) legacyBackupName.renameTo(baseName);
        if (newName.exists() && baseName.exists()) newName.delete();
        return new FileInputStream(baseName);
    }

    public long getLastModifiedTime() { return baseName.lastModified(); }

    public byte[] readFully() throws IOException {
        try (FileInputStream in = openRead()) { return in.readAllBytes(); }
    }

    @Override public String toString() { return "AtomicFile[" + baseName + "]"; }
}
