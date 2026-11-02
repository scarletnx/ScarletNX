// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;

/** A held connection to a provider. */
public class ContentProviderClient implements AutoCloseable {
    private final ContentProvider provider;

    ContentProviderClient(ContentProvider provider) { this.provider = provider; }
    public ContentProvider getLocalContentProvider() { return provider; }
    public Cursor query(Uri url, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return provider.query(url, projection, selection, selectionArgs, sortOrder);
    }
    public Cursor query(Uri url, String[] projection, String selection, String[] selectionArgs, String sortOrder,
                        android.os.CancellationSignal cancellationSignal) {
        return provider.query(url, projection, selection, selectionArgs, sortOrder, cancellationSignal);
    }
    public Uri insert(Uri url, ContentValues initialValues) { return provider.insert(url, initialValues); }
    public int delete(Uri url, String selection, String[] selectionArgs) { return provider.delete(url, selection, selectionArgs); }
    public int update(Uri url, ContentValues values, String selection, String[] selectionArgs) { return provider.update(url, values, selection, selectionArgs); }
    public Bundle call(String method, String arg, Bundle extras) { return provider.call(method, arg, extras); }
    public String getType(Uri url) { return provider.getType(url); }
    @Override public void close() {}
    @Deprecated public boolean release() { return true; }
}
