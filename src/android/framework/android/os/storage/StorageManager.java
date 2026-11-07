// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os.storage;

import android.net.Uri;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/** The storage volumes: one, the primary shared storage. */
public class StorageManager {
    public static final UUID UUID_DEFAULT = UUID.fromString("41217664-9172-527a-b3d5-edabb50a7d69");
    public static final String ACTION_MANAGE_STORAGE = "android.os.storage.action.MANAGE_STORAGE";

    private static final StorageVolume PRIMARY = new StorageVolume();

    public StorageManager() {}

    public StorageVolume getPrimaryStorageVolume() { return PRIMARY; }
    public List<StorageVolume> getStorageVolumes() { List<StorageVolume> l = new ArrayList<>(); l.add(PRIMARY); return l; }
    public StorageVolume getStorageVolume(File file) { return PRIMARY; }

    /** The volume of a MediaStore item, which is the primary volume. */
    public StorageVolume getStorageVolume(Uri uri) {
        if (!"media".equals(uri.getAuthority())) throw new IllegalArgumentException("Unknown volume for " + uri);
        return PRIMARY;
    }

    public UUID getUuidForPath(File path) { return UUID_DEFAULT; }
    public long getAllocatableBytes(UUID storageUuid) throws java.io.IOException { return android.os.Environment.getDataDirectory().getUsableSpace(); }
    public void allocateBytes(UUID storageUuid, long bytes) throws java.io.IOException {}
    public long getCacheQuotaBytes(UUID storageUuid) { return 64L * 1024 * 1024; }
    public boolean isEncrypted(File file) { return false; }
}
