// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** A batch operation's result. */
public class ContentProviderResult {
    public final android.net.Uri uri;
    public final Integer count;
    public ContentProviderResult(android.net.Uri uri) { this.uri = uri; count = null; }
    public ContentProviderResult(int count) { uri = null; this.count = count; }
}
