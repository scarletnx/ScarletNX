// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.provider;

import android.net.Uri;

/** Document URIs of document providers: the app's own, as there are no others. */
public final class DocumentsContract {
    public static final String EXTRA_INITIAL_URI = "android.provider.extra.INITIAL_URI";

    public static final class Document {
        public static final String COLUMN_DOCUMENT_ID = "document_id", COLUMN_MIME_TYPE = "mime_type",
            COLUMN_DISPLAY_NAME = "_display_name", COLUMN_LAST_MODIFIED = "last_modified", COLUMN_FLAGS = "flags",
            COLUMN_SIZE = "_size", MIME_TYPE_DIR = "vnd.android.document/directory";
    }

    public static boolean isDocumentUri(android.content.Context context, Uri uri) { return uri != null && uri.getPathSegments().size() >= 2 && "document".equals(uri.getPathSegments().get(0)); }
    public static boolean isTreeUri(Uri uri) { return uri != null && uri.getPathSegments().size() >= 2 && "tree".equals(uri.getPathSegments().get(0)); }
    public static String getDocumentId(Uri documentUri) { java.util.List<String> p = documentUri.getPathSegments(); return p.size() >= 2 ? p.get(p.size() - 1) : null; }
    public static String getTreeDocumentId(Uri documentUri) { java.util.List<String> p = documentUri.getPathSegments(); return p.size() >= 2 ? p.get(1) : null; }
    public static Uri buildDocumentUri(String authority, String documentId) { return new Uri.Builder().scheme("content").authority(authority).appendPath("document").appendPath(documentId).build(); }
    public static Uri buildDocumentUriUsingTree(Uri treeUri, String documentId) {
        return new Uri.Builder().scheme("content").authority(treeUri.getAuthority()).appendPath("tree").appendPath(getTreeDocumentId(treeUri)).appendPath("document").appendPath(documentId).build();
    }
    public static Uri buildChildDocumentsUriUsingTree(Uri treeUri, String parentDocumentId) {
        return new Uri.Builder().scheme("content").authority(treeUri.getAuthority()).appendPath("tree").appendPath(getTreeDocumentId(treeUri)).appendPath("document").appendPath(parentDocumentId).appendPath("children").build();
    }
    public static Uri createDocument(android.content.ContentResolver content, Uri parentDocumentUri, String mimeType, String displayName) { return null; }
    public static boolean deleteDocument(android.content.ContentResolver content, Uri documentUri) { return false; }
    public static Uri renameDocument(android.content.ContentResolver content, Uri documentUri, String displayName) { return null; }
}
