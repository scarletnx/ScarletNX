// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.provider;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.graphics.Point;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * A provider of documents (the Storage Access Framework's): the app gives its roots, documents and their children,
 * and this class answers the content URIs of DocumentsContract with them (content://authority/root,
 * /document/ID, /document/ID/children, and the same under /tree/ID). The document calls that change something
 * (create, delete, rename, copy, move, remove) are unsupported until the app implements them.
 */
public abstract class DocumentsProvider extends ContentProvider {
    public DocumentsProvider() {}

    public abstract Cursor queryRoots(String[] projection) throws FileNotFoundException;
    public abstract Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException;
    public abstract Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
        throws FileNotFoundException;
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, Bundle queryArgs)
        throws FileNotFoundException {
        return queryChildDocuments(parentDocumentId, projection, queryArgs != null ? queryArgs.getString("android:query-arg-sql-sort-order") : null);
    }
    public abstract ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
        throws FileNotFoundException;

    public Cursor queryRecentDocuments(String rootId, String[] projection) throws FileNotFoundException {
        throw new UnsupportedOperationException("Recent not supported");
    }
    public Cursor querySearchDocuments(String rootId, String query, String[] projection) throws FileNotFoundException {
        throw new UnsupportedOperationException("Search not supported");
    }
    public boolean isChildDocument(String parentDocumentId, String documentId) { return false; }
    public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        throw new UnsupportedOperationException("Create not supported");
    }
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        throw new UnsupportedOperationException("Rename not supported");
    }
    public void deleteDocument(String documentId) throws FileNotFoundException {
        throw new UnsupportedOperationException("Delete not supported");
    }
    public String copyDocument(String sourceDocumentId, String targetParentDocumentId) throws FileNotFoundException {
        throw new UnsupportedOperationException("Copy not supported");
    }
    public String moveDocument(String sourceDocumentId, String sourceParentDocumentId, String targetParentDocumentId)
        throws FileNotFoundException {
        throw new UnsupportedOperationException("Move not supported");
    }
    public void removeDocument(String documentId, String parentDocumentId) throws FileNotFoundException {
        throw new UnsupportedOperationException("Remove not supported");
    }
    public AssetFileDescriptor openDocumentThumbnail(String documentId, Point sizeHint, CancellationSignal signal)
        throws FileNotFoundException {
        throw new UnsupportedOperationException("Thumbnails not supported");
    }
    /** a document's MIME type: its COLUMN_MIME_TYPE in queryDocument */
    public String getDocumentType(String documentId) throws FileNotFoundException {
        try (Cursor c = queryDocument(documentId, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE);
                return i >= 0 ? c.getString(i) : null;
            }
        }
        return null;
    }

    /* the document of a URI: .../document/ID, or the tree's own ID for .../tree/ID; null for another */
    private static String documentOf(Uri uri) {
        List<String> p = uri.getPathSegments();
        if (p.size() >= 2 && "document".equals(p.get(0))) return p.get(1);
        if (p.size() >= 4 && "tree".equals(p.get(0)) && "document".equals(p.get(2))) return p.get(3);
        if (p.size() == 2 && "tree".equals(p.get(0))) return p.get(1);
        return null;
    }
    private static boolean children(Uri uri) {
        List<String> p = uri.getPathSegments();
        return !p.isEmpty() && "children".equals(p.get(p.size() - 1));
    }

    @Override public final Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        try {
            List<String> p = uri.getPathSegments();
            if (p.size() == 1 && "root".equals(p.get(0))) return queryRoots(projection);
            String document = documentOf(uri);
            if (document == null) throw new UnsupportedOperationException("Unsupported Uri " + uri);
            return children(uri) ? queryChildDocuments(document, projection, sortOrder) : queryDocument(document, projection);
        } catch (FileNotFoundException e) {
            android.util.Log.w("DocumentsProvider", "Failed during query", e);
            return null;
        }
    }
    @Override public final String getType(Uri uri) {
        String document = documentOf(uri);
        if (document == null || children(uri)) return null;
        try {
            return getDocumentType(document);
        } catch (FileNotFoundException e) {
            return null;
        }
    }
    @Override public final Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("Insert not supported"); }
    @Override public final int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("Delete not supported"); }
    @Override public final int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Update not supported");
    }
    @Override public final ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return openFile(uri, mode, null);
    }
    @Override public final ParcelFileDescriptor openFile(Uri uri, String mode, CancellationSignal signal) throws FileNotFoundException {
        String document = documentOf(uri);
        if (document == null) throw new FileNotFoundException("No document in " + uri);
        return openDocument(document, mode, signal);
    }
}
