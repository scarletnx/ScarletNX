// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content;

import android.net.Uri;

/** Content URIs with an ID at their end. */
public class ContentUris {
    public static long parseId(Uri contentUri) {
        String last = contentUri.getLastPathSegment();
        return last == null ? -1 : Long.parseLong(last);
    }
    public static Uri.Builder appendId(Uri.Builder builder, long id) { return builder.appendEncodedPath(String.valueOf(id)); }
    public static Uri withAppendedId(Uri contentUri, long id) { return appendId(contentUri.buildUpon(), id).build(); }
    public static Uri removeId(Uri contentUri) {
        java.util.List<String> segments = contentUri.getPathSegments();
        Uri.Builder b = contentUri.buildUpon().path(null);
        for (int i = 0; i < segments.size() - 1; ++i) b.appendPath(segments.get(i));
        return b.build();
    }
}
