// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.database.ContentObserver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;

/**
 * Content by URI: a content: URI goes to the app's provider of its authority, a file: URI to the file. There is no
 * other process, so a URI of another app's provider has no provider. Observers hear of the changes made here.
 */
public abstract class ContentResolver {
    public static final String SCHEME_CONTENT = "content", SCHEME_ANDROID_RESOURCE = "android.resource", SCHEME_FILE = "file";
    public static final String CURSOR_ITEM_BASE_TYPE = "vnd.android.cursor.item", CURSOR_DIR_BASE_TYPE = "vnd.android.cursor.dir";
    public static final String QUERY_ARG_SQL_SELECTION = "android:query-arg-sql-selection",
        QUERY_ARG_SQL_SELECTION_ARGS = "android:query-arg-sql-selection-args", QUERY_ARG_SQL_SORT_ORDER = "android:query-arg-sql-sort-order",
        QUERY_ARG_LIMIT = "android:query-arg-limit", QUERY_ARG_OFFSET = "android:query-arg-offset";
    public static final int NOTIFY_SYNC_TO_NETWORK = 1, NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS = 2, NOTIFY_INSERT = 4,
        NOTIFY_UPDATE = 8, NOTIFY_DELETE = 16;
    public static final String SYNC_EXTRAS_MANUAL = "force", SYNC_EXTRAS_EXPEDITED = "expedited";

    private final Context context;
    private static final ArrayList<Object[]> observers = new ArrayList<>(); // uri, descendants, observer

    public ContentResolver(Context context) { this.context = context; }

    /** The app's provider of an authority, made and attached when first asked for. */
    protected abstract ContentProvider provider(String authority);

    private ContentProvider providerFor(Uri uri) {
        if (!SCHEME_CONTENT.equals(uri.getScheme())) return null;
        return provider(uri.getAuthority());
    }

    public final String getType(Uri url) {
        ContentProvider p = providerFor(url);
        return p != null ? p.getType(url) : null;
    }
    public String[] getStreamTypes(Uri url, String mimeTypeFilter) {
        ContentProvider p = providerFor(url);
        return p != null ? p.getStreamTypes(url, mimeTypeFilter) : null;
    }
    public final Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return query(uri, projection, selection, selectionArgs, sortOrder, null);
    }
    public final Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder, CancellationSignal cancellationSignal) {
        ContentProvider p = providerFor(uri);
        return p != null ? p.query(uri, projection, selection, selectionArgs, sortOrder, cancellationSignal) : null;
    }
    public final Cursor query(Uri uri, String[] projection, Bundle queryArgs, CancellationSignal cancellationSignal) {
        ContentProvider p = providerFor(uri);
        return p != null ? p.query(uri, projection, queryArgs, cancellationSignal) : null;
    }
    public final Uri insert(Uri url, ContentValues values) {
        ContentProvider p = providerFor(url);
        if (p == null) throw new IllegalArgumentException("Unknown URL " + url);
        return p.insert(url, values);
    }
    public final Uri insert(Uri url, ContentValues values, Bundle extras) { return insert(url, values); }
    public final int bulkInsert(Uri url, ContentValues[] values) {
        ContentProvider p = providerFor(url);
        if (p == null) throw new IllegalArgumentException("Unknown URL " + url);
        return p.bulkInsert(url, values);
    }
    public final int delete(Uri url, String where, String[] selectionArgs) {
        ContentProvider p = providerFor(url);
        if (p == null) throw new IllegalArgumentException("Unknown URL " + url);
        return p.delete(url, where, selectionArgs);
    }
    public final int update(Uri uri, ContentValues values, String where, String[] selectionArgs) {
        ContentProvider p = providerFor(uri);
        if (p == null) throw new IllegalArgumentException("Unknown URI " + uri);
        return p.update(uri, values, where, selectionArgs);
    }
    public final Bundle call(Uri uri, String method, String arg, Bundle extras) {
        ContentProvider p = providerFor(uri);
        if (p == null) throw new IllegalArgumentException("Unknown URI " + uri);
        return p.call(method, arg, extras);
    }
    public final Bundle call(String authority, String method, String arg, Bundle extras) {
        ContentProvider p = provider(authority);
        if (p == null) throw new IllegalArgumentException("Unknown authority " + authority);
        return p.call(authority, method, arg, extras);
    }
    public final ContentProviderClient acquireContentProviderClient(Uri uri) {
        ContentProvider p = providerFor(uri);
        return p != null ? new ContentProviderClient(p) : null;
    }
    public final ContentProviderClient acquireContentProviderClient(String name) {
        ContentProvider p = provider(name);
        return p != null ? new ContentProviderClient(p) : null;
    }
    public final ContentProviderClient acquireUnstableContentProviderClient(Uri uri) { return acquireContentProviderClient(uri); }
    public final ContentProviderClient acquireUnstableContentProviderClient(String name) { return acquireContentProviderClient(name); }
    public final Uri canonicalize(Uri url) { ContentProvider p = providerFor(url); return p != null ? p.canonicalize(url) : null; }
    public final Uri uncanonicalize(Uri url) { ContentProvider p = providerFor(url); return p != null ? p.uncanonicalize(url) : url; }

    public final InputStream openInputStream(Uri uri) throws FileNotFoundException {
        if (SCHEME_FILE.equals(uri.getScheme())) return new FileInputStream(new File(uri.getPath()));
        ParcelFileDescriptor fd = openFileDescriptor(uri, "r");
        return new ParcelFileDescriptor.AutoCloseInputStream(fd);
    }
    public final OutputStream openOutputStream(Uri uri) throws FileNotFoundException { return openOutputStream(uri, "w"); }
    public final OutputStream openOutputStream(Uri uri, String mode) throws FileNotFoundException {
        if (SCHEME_FILE.equals(uri.getScheme())) return new FileOutputStream(new File(uri.getPath()), mode.contains("a"));
        return new ParcelFileDescriptor.AutoCloseOutputStream(openFileDescriptor(uri, mode));
    }
    public final ParcelFileDescriptor openFileDescriptor(Uri uri, String mode) throws FileNotFoundException {
        return openFileDescriptor(uri, mode, null);
    }
    public final ParcelFileDescriptor openFileDescriptor(Uri uri, String mode, CancellationSignal cancellationSignal) throws FileNotFoundException {
        if (SCHEME_FILE.equals(uri.getScheme())) return ParcelFileDescriptor.open(new File(uri.getPath()), ParcelFileDescriptor.parseMode(mode));
        ContentProvider p = providerFor(uri);
        if (p == null) throw new FileNotFoundException("No content provider: " + uri);
        return p.openFile(uri, mode, cancellationSignal);
    }
    public final android.content.res.AssetFileDescriptor openAssetFileDescriptor(Uri uri, String mode) throws FileNotFoundException {
        return new android.content.res.AssetFileDescriptor(openFileDescriptor(uri, mode), 0, -1);
    }
    public final android.content.res.AssetFileDescriptor openTypedAssetFileDescriptor(Uri uri, String mimeType, Bundle opts) throws FileNotFoundException {
        return openAssetFileDescriptor(uri, "r");
    }
    public void takePersistableUriPermission(Uri uri, int modeFlags) {}
    public void releasePersistableUriPermission(Uri uri, int modeFlags) {}
    public java.util.List<Object> getPersistedUriPermissions() { return java.util.Collections.emptyList(); }

    public final void registerContentObserver(Uri uri, boolean notifyForDescendants, ContentObserver observer) {
        synchronized (observers) { observers.add(new Object[] {uri, notifyForDescendants, observer}); }
    }
    public final void unregisterContentObserver(ContentObserver observer) {
        synchronized (observers) { observers.removeIf(o -> o[2] == observer); }
    }
    public void notifyChange(Uri uri, ContentObserver observer) { notifyChange(uri, observer, 0); }
    public void notifyChange(Uri uri, ContentObserver observer, boolean syncToNetwork) { notifyChange(uri, observer, 0); }
    public void notifyChange(Uri uri, ContentObserver observer, int flags) {
        ArrayList<ContentObserver> to = new ArrayList<>();
        String u = uri.toString();
        synchronized (observers) {
            for (Object[] o : observers) {
                String r = o[0].toString();
                boolean match = u.equals(r) || ((Boolean) o[1] && u.startsWith(r.endsWith("/") ? r : r + "/"))
                    || ((flags & NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0 && r.startsWith(u.endsWith("/") ? u : u + "/"));
                if (match && o[2] != observer) to.add((ContentObserver) o[2]);
            }
        }
        for (ContentObserver o : to) o.dispatchChange(false, uri);
    }
    public void notifyChange(java.util.Collection<Uri> uris, ContentObserver observer, int flags) { for (Uri u : uris) notifyChange(u, observer, flags); }
    public static void requestSync(android.accounts.Account account, String authority, Bundle extras) {}
    public static boolean getMasterSyncAutomatically() { return true; }
}
