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
    /** the document of .../document/ID or .../tree/TREE/document/ID, as Android's: an IllegalArgumentException for another */
    public static String getDocumentId(Uri documentUri) {
        java.util.List<String> p = documentUri.getPathSegments();
        if (p.size() >= 2 && "document".equals(p.get(0))) return p.get(1);
        if (p.size() >= 4 && "tree".equals(p.get(0)) && "document".equals(p.get(2))) return p.get(3);
        throw new IllegalArgumentException("Invalid URI: " + documentUri);
    }
    public static String getTreeDocumentId(Uri documentUri) { java.util.List<String> p = documentUri.getPathSegments(); return p.size() >= 2 ? p.get(1) : null; }
    public static Uri buildRootUri(String authority, String rootId) { return new Uri.Builder().scheme("content").authority(authority).appendPath("root").appendPath(rootId).build(); }
    public static Uri buildDocumentUri(String authority, String documentId) { return new Uri.Builder().scheme("content").authority(authority).appendPath("document").appendPath(documentId).build(); }
    public static Uri buildDocumentUriUsingTree(Uri treeUri, String documentId) {
        return new Uri.Builder().scheme("content").authority(treeUri.getAuthority()).appendPath("tree").appendPath(getTreeDocumentId(treeUri)).appendPath("document").appendPath(documentId).build();
    }
    public static Uri buildChildDocumentsUriUsingTree(Uri treeUri, String parentDocumentId) {
        return new Uri.Builder().scheme("content").authority(treeUri.getAuthority()).appendPath("tree").appendPath(getTreeDocumentId(treeUri)).appendPath("document").appendPath(parentDocumentId).appendPath("children").build();
    }
    public static Uri buildChildDocumentsUri(String authority, String parentDocumentId) {
        return new Uri.Builder().scheme("content").authority(authority).appendPath("document").appendPath(parentDocumentId).appendPath("children").build();
    }
    public static Uri buildTreeDocumentUri(String authority, String documentId) {
        return new Uri.Builder().scheme("content").authority(authority).appendPath("tree").appendPath(documentId).build();
    }
    // the calls of a document provider, through the content resolver, as Android's: the method, and the URIs in extras
    static final String METHOD_CREATE_DOCUMENT = "android:createDocument", METHOD_RENAME_DOCUMENT = "android:renameDocument",
        METHOD_DELETE_DOCUMENT = "android:deleteDocument", METHOD_COPY_DOCUMENT = "android:copyDocument",
        METHOD_MOVE_DOCUMENT = "android:moveDocument", METHOD_REMOVE_DOCUMENT = "android:removeDocument",
        METHOD_IS_CHILD_DOCUMENT = "android:isChildDocument";
    static final String EXTRA_URI = "uri", EXTRA_PARENT_URI = "parentUri", EXTRA_TARGET_URI = "android.content.extra.TARGET_URI",
        EXTRA_RESULT = "result";

    private static android.os.Bundle call(android.content.ContentResolver content, Uri uri, String method, android.os.Bundle in)
            throws java.io.FileNotFoundException {
        try {
            return content.call(uri.getAuthority(), method, null, in);
        } catch (IllegalArgumentException | UnsupportedOperationException | IllegalStateException e) {
            android.util.Log.w("DocumentsContract", "Failed " + method + " on " + uri, e);
            return null;
        }
    }
    private static Uri uriOf(android.os.Bundle out) { return out != null ? (Uri) out.getParcelable(EXTRA_URI) : null; }

    public static Uri createDocument(android.content.ContentResolver content, Uri parentDocumentUri, String mimeType, String displayName)
            throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, parentDocumentUri);
        in.putString(Document.COLUMN_MIME_TYPE, mimeType);
        in.putString(Document.COLUMN_DISPLAY_NAME, displayName);
        return uriOf(call(content, parentDocumentUri, METHOD_CREATE_DOCUMENT, in));
    }
    public static Uri renameDocument(android.content.ContentResolver content, Uri documentUri, String displayName)
            throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, documentUri);
        in.putString(Document.COLUMN_DISPLAY_NAME, displayName);
        android.os.Bundle out = call(content, documentUri, METHOD_RENAME_DOCUMENT, in);
        if (out == null) return null;
        Uri renamed = uriOf(out);
        return renamed != null ? renamed : documentUri;
    }
    public static boolean deleteDocument(android.content.ContentResolver content, Uri documentUri) throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, documentUri);
        return call(content, documentUri, METHOD_DELETE_DOCUMENT, in) != null;
    }
    public static boolean removeDocument(android.content.ContentResolver content, Uri documentUri, Uri parentDocumentUri)
            throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, documentUri);
        in.putParcelable(EXTRA_PARENT_URI, parentDocumentUri);
        return call(content, documentUri, METHOD_REMOVE_DOCUMENT, in) != null;
    }
    public static Uri copyDocument(android.content.ContentResolver content, Uri sourceDocumentUri, Uri targetParentDocumentUri)
            throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, sourceDocumentUri);
        in.putParcelable(EXTRA_TARGET_URI, targetParentDocumentUri);
        return uriOf(call(content, sourceDocumentUri, METHOD_COPY_DOCUMENT, in));
    }
    public static Uri moveDocument(android.content.ContentResolver content, Uri sourceDocumentUri, Uri sourceParentDocumentUri,
                                   Uri targetParentDocumentUri) throws java.io.FileNotFoundException {
        android.os.Bundle in = new android.os.Bundle();
        in.putParcelable(EXTRA_URI, sourceDocumentUri);
        in.putParcelable(EXTRA_PARENT_URI, sourceParentDocumentUri);
        in.putParcelable(EXTRA_TARGET_URI, targetParentDocumentUri);
        return uriOf(call(content, sourceDocumentUri, METHOD_MOVE_DOCUMENT, in));
    }
    /** a document of a tree stays in the tree, else the provider's own URI of it */
    static Uri buildDocumentUriMaybeUsingTree(Uri base, String documentId) {
        return isTreeUri(base) ? buildDocumentUriUsingTree(base, documentId) : buildDocumentUri(base.getAuthority(), documentId);
    }
}
