// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2013 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import android.graphics.Bitmap;
import java.util.HashMap;
import java.util.HashSet;

/** Edits the metadata of a remote control client, by key; apply() sends it. */
@Deprecated
public abstract class MediaMetadataEditor {
    public static final int BITMAP_KEY_ARTWORK = 100, RATING_KEY_BY_OTHERS = 101, RATING_KEY_BY_USER = 0x10000001;

    protected boolean mApplied;
    protected final HashMap<Integer, Object> values = new HashMap<>();
    private final HashSet<Integer> editable = new HashSet<>();

    protected MediaMetadataEditor() {}

    private void check() { if (mApplied) throw new IllegalStateException("Can't edit a previously applied MediaMetadataEditor"); }

    public synchronized void clear() { check(); values.clear(); }
    public synchronized void addEditableKey(int key) { check(); editable.add(key); }
    public synchronized void removeEditableKeys() { check(); editable.clear(); }
    public synchronized int[] getEditableKeys() { return editable.isEmpty() ? null : editable.stream().mapToInt(Integer::intValue).toArray(); }

    public synchronized MediaMetadataEditor putString(int key, String value) { check(); values.put(key, value); return this; }
    public synchronized MediaMetadataEditor putLong(int key, long value) { check(); values.put(key, value); return this; }
    public synchronized MediaMetadataEditor putBitmap(int key, Bitmap bitmap) { check(); values.put(key, bitmap); return this; }
    public synchronized MediaMetadataEditor putObject(int key, Object value) { check(); values.put(key, value); return this; }

    public synchronized long getLong(int key, long defaultValue) { Object v = values.get(key); return v instanceof Long l ? l : defaultValue; }
    public synchronized String getString(int key, String defaultValue) { Object v = values.get(key); return v instanceof String s ? s : defaultValue; }
    public synchronized Bitmap getBitmap(int key, Bitmap defaultValue) { Object v = values.get(key); return v instanceof Bitmap b ? b : defaultValue; }
    public synchronized Object getObject(int key, Object defaultValue) { return values.getOrDefault(key, defaultValue); }

    public abstract void apply();
}
