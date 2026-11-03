// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The system's log of crashes and events: nothing reads it here, so it keeps no entries. */
public class DropBoxManager {
    public static final String ACTION_DROPBOX_ENTRY_ADDED = "android.intent.action.DROPBOX_ENTRY_ADDED",
        EXTRA_TAG = "tag", EXTRA_TIME = "time", EXTRA_DROPPED_COUNT = "android.os.extra.DROPPED_COUNT";
    public static final int IS_EMPTY = 1, IS_TEXT = 2, IS_GZIPPED = 4;

    public DropBoxManager() {}

    public void addText(String tag, String data) {}
    public void addData(String tag, byte[] data, int flags) {}
    public void addFile(String tag, java.io.File file, int flags) throws java.io.IOException {}
    public boolean isTagEnabled(String tag) { return false; }
}
