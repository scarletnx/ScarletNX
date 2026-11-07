// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content;

import android.content.pm.ProviderInfo;
import android.content.res.AssetFileDescriptor;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import java.io.FileNotFoundException;

/** Content for a ContentResolver: a provider of the app, reached in the process. */
public abstract class ContentProvider implements ComponentCallbacks2 {
    private Context context;
    private String authority;
    private String readPermission, writePermission;
    private boolean exported;

    public ContentProvider() {}

    public void attachInfo(Context context, ProviderInfo info) {
        if (this.context != null) return;
        this.context = context;
        if (info != null) {
            authority = info.authority;
            readPermission = info.readPermission;
            writePermission = info.writePermission;
            exported = info.exported;
        }
        onCreate();
    }

    public final Context getContext() { return context; }
    public final Context requireContext() {
        if (context == null) throw new IllegalStateException("Cannot find context from the provider.");
        return context;
    }
    public final String getCallingPackage() { return context != null ? context.getPackageName() : null; }
    public final String getCallingPackageUnchecked() { return getCallingPackage(); }
    public final String getCallingAttributionTag() { return null; }
    protected final void setReadPermission(String permission) { readPermission = permission; }
    public final String getReadPermission() { return readPermission; }
    protected final void setWritePermission(String permission) { writePermission = permission; }
    public final String getWritePermission() { return writePermission; }
    protected final void setAuthorities(String authorities) { authority = authorities; }
    public final String getAuthority() { return authority; }

    public abstract boolean onCreate();
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public void onLowMemory() {}
    @Override public void onTrimMemory(int level) {}
    public abstract Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder);
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder, CancellationSignal cancellationSignal) {
        return query(uri, projection, selection, selectionArgs, sortOrder);
    }
    public Cursor query(Uri uri, String[] projection, Bundle queryArgs, CancellationSignal cancellationSignal) {
        String selection = queryArgs != null ? queryArgs.getString(ContentResolver.QUERY_ARG_SQL_SELECTION) : null;
        String[] args = queryArgs != null ? queryArgs.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS) : null;
        String sort = queryArgs != null ? queryArgs.getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER) : null;
        return query(uri, projection, selection, args, sort, cancellationSignal);
    }
    public abstract String getType(Uri uri);
    public String getTypeAnonymous(Uri uri) { return getType(uri); }
    public Uri canonicalize(Uri url) { return null; }
    public Uri uncanonicalize(Uri url) { return url; }
    public boolean refresh(Uri uri, Bundle extras, CancellationSignal cancellationSignal) { return false; }
    public abstract Uri insert(Uri uri, ContentValues values);
    public Uri insert(Uri uri, ContentValues values, Bundle extras) { return insert(uri, values); }
    public int bulkInsert(Uri uri, ContentValues[] values) { int n = 0; for (ContentValues v : values) if (insert(uri, v) != null) ++n; return n; }
    public abstract int delete(Uri uri, String selection, String[] selectionArgs);
    public int delete(Uri uri, Bundle extras) { return delete(uri, extras != null ? extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION) : null, null); }
    public abstract int update(Uri uri, ContentValues values, String selection, String[] selectionArgs);
    public int update(Uri uri, ContentValues values, Bundle extras) { return update(uri, values, extras != null ? extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION) : null, null); }
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException { throw new FileNotFoundException("No files supported by provider at " + uri); }
    public ParcelFileDescriptor openFile(Uri uri, String mode, CancellationSignal signal) throws FileNotFoundException { return openFile(uri, mode); }
    public AssetFileDescriptor openAssetFile(Uri uri, String mode) throws FileNotFoundException {
        ParcelFileDescriptor fd = openFile(uri, mode);
        return fd != null ? new AssetFileDescriptor(fd, 0, -1) : null;
    }
    public AssetFileDescriptor openAssetFile(Uri uri, String mode, CancellationSignal signal) throws FileNotFoundException { return openAssetFile(uri, mode); }
    public String[] getStreamTypes(Uri uri, String mimeTypeFilter) { return null; }
    public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeTypeFilter, Bundle opts) throws FileNotFoundException { return openAssetFile(uri, "r"); }
    public Bundle call(String method, String arg, Bundle extras) { return null; }
    public Bundle call(String authority, String method, String arg, Bundle extras) { return call(method, arg, extras); }
    public void shutdown() {}
    public ContentProviderResult[] applyBatch(java.util.ArrayList<ContentProviderOperation> operations) { return new ContentProviderResult[0]; }
    protected boolean isTemporary() { return false; }
}
