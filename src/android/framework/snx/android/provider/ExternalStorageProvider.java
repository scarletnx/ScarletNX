// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.provider;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;

/**
 * The device's shared storage as the system's documents provider shows it (com.android.externalstorage.documents, the
 * one DocumentsUI's pickers answer with): the files under Environment.getExternalStorageDirectory(), a document's ID
 * "primary:" and its path there, as Android's, so that an app that turns an ID into a path (the primary volume's
 * folder, then the path) finds the same file. A folder the picker gives that is elsewhere on the host is linked into
 * the shared storage (shared), as if the user had picked it there.
 */
public final class ExternalStorageProvider extends DocumentsProvider {
    public static final String AUTHORITY = "com.android.externalstorage.documents";
    private static final String ROOT = "primary";
    private static final String[] DOCUMENT_COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_DISPLAY_NAME, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE};
    private static final String[] ROOT_COLUMNS = {"root_id", "flags", "title", "document_id", "available_bytes"};
    // Document.FLAG_SUPPORTS_WRITE 2, FLAG_SUPPORTS_DELETE 4, FLAG_DIR_SUPPORTS_CREATE 8
    private static final int FILE_FLAGS = 2 | 4, DIR_FLAGS = 4 | 8;

    private static File root() { return android.os.Environment.getExternalStorageDirectory().getAbsoluteFile(); }

    /** the document ID of a file in the shared storage, null for one elsewhere */
    public static String documentId(File file) {
        String path = file.getAbsolutePath(), root = root().getPath();
        if (path.equals(root)) return ROOT + ":";
        if (!path.startsWith(root + "/")) return null;
        return ROOT + ":" + path.substring(root.length() + 1);
    }
    /**
     * A host file as a file of the shared storage: itself when it is in it, else a link to it in the storage's top
     * folder, by its name (another name when that one is taken by something else)
     */
    public static File shared(File file) {
        File f = file.getAbsoluteFile();
        if (documentId(f) != null) return f;
        File top = root();
        top.mkdirs();
        for (int i = 0; i < 100; ++i) {
            File link = new File(top, i == 0 ? f.getName() : f.getName() + " (" + i + ")");
            java.nio.file.Path lp = link.toPath();
            try {
                if (java.nio.file.Files.isSymbolicLink(lp)) {
                    if (java.nio.file.Files.readSymbolicLink(lp).equals(f.toPath())) return link;
                    continue;
                }
                if (link.exists()) continue;
                java.nio.file.Files.createSymbolicLink(lp, f.toPath());
                return link;
            } catch (java.io.IOException e) {
                return null;
            }
        }
        return null;
    }
    private static File fileOf(String documentId) throws FileNotFoundException {
        int colon = documentId != null ? documentId.indexOf(':') : -1;
        if (colon < 0 || !ROOT.equals(documentId.substring(0, colon))) throw new FileNotFoundException("No root for " + documentId);
        String rel = documentId.substring(colon + 1);
        return rel.isEmpty() ? root() : new File(root(), rel);
    }

    @Override public boolean onCreate() { return true; }

    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        File root = root();
        // Root.FLAG_SUPPORTS_CREATE 1, FLAG_LOCAL_ONLY 2, FLAG_SUPPORTS_IS_CHILD 16
        c.newRow().add("root_id", ROOT).add("flags", 1 | 2 | 16).add("title", "Internal storage")
            .add("document_id", documentId(root)).add("available_bytes", root.getUsableSpace());
        return c;
    }
    private static void row(MatrixCursor c, File f) {
        boolean dir = f.isDirectory();
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, documentId(f))
            .add(Document.COLUMN_MIME_TYPE, dir ? Document.MIME_TYPE_DIR : mimeType(f.getName()))
            .add(Document.COLUMN_DISPLAY_NAME, f.getName().isEmpty() ? "/" : f.getName())
            .add(Document.COLUMN_LAST_MODIFIED, f.lastModified()).add(Document.COLUMN_FLAGS, dir ? DIR_FLAGS : FILE_FLAGS)
            .add(Document.COLUMN_SIZE, dir ? null : f.length());
    }
    private static String mimeType(String name) {
        int dot = name.lastIndexOf('.');
        String type = dot >= 0 ? android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase()) : null;
        return type != null ? type : "application/octet-stream";
    }
    @Override public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        File f = fileOf(documentId);
        if (!f.exists()) throw new FileNotFoundException("Missing file for " + documentId + " at " + f);
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        row(c, f);
        return c;
    }
    @Override public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder) throws FileNotFoundException {
        File dir = fileOf(parentDocumentId);
        if (!dir.isDirectory()) throw new FileNotFoundException("Not a directory: " + parentDocumentId);
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File[] files = dir.listFiles();
        if (files != null) {
            java.util.Arrays.sort(files);
            for (File f : files) row(c, f);
        }
        return c;
    }
    @Override public boolean isChildDocument(String parentDocumentId, String documentId) {
        try {
            String parent = fileOf(parentDocumentId).getPath(), child = fileOf(documentId).getPath();
            return child.startsWith(parent.endsWith("/") ? parent : parent + "/");
        } catch (FileNotFoundException e) {
            return false;
        }
    }
    @Override public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileOf(documentId), ParcelFileDescriptor.parseMode(mode));
    }
    @Override public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        File f = new File(fileOf(parentDocumentId), displayName);
        boolean made;
        try {
            made = Document.MIME_TYPE_DIR.equals(mimeType) ? f.mkdir() : f.createNewFile();
        } catch (java.io.IOException e) {
            throw new IllegalStateException("Failed to create " + f, e);
        }
        if (!made) throw new IllegalStateException("Failed to create " + f);
        return documentId(f);
    }
    /** a file, or a folder with what it holds */
    @Override public void deleteDocument(String documentId) throws FileNotFoundException {
        File f = fileOf(documentId);
        if (!delete(f)) throw new IllegalStateException("Failed to delete " + f);
    }
    private static boolean delete(File f) {
        File[] children = java.nio.file.Files.isSymbolicLink(f.toPath()) ? null : f.listFiles();
        if (children != null) for (File c : children) delete(c);
        return f.delete();
    }
    @Override public String copyDocument(String sourceDocumentId, String targetParentDocumentId) throws FileNotFoundException {
        File from = fileOf(sourceDocumentId), to = new File(fileOf(targetParentDocumentId), from.getName());
        try {
            java.nio.file.Files.copy(from.toPath(), to.toPath());
        } catch (java.io.IOException e) {
            throw new IllegalStateException("Failed to copy " + from, e);
        }
        return documentId(to);
    }
    @Override public String moveDocument(String sourceDocumentId, String sourceParentDocumentId, String targetParentDocumentId)
            throws FileNotFoundException {
        File from = fileOf(sourceDocumentId), to = new File(fileOf(targetParentDocumentId), from.getName());
        if (!from.renameTo(to)) throw new IllegalStateException("Failed to move " + from);
        return documentId(to);
    }
    @Override public void removeDocument(String documentId, String parentDocumentId) throws FileNotFoundException { deleteDocument(documentId); }
    @Override public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        File f = fileOf(documentId), to = new File(f.getParentFile(), displayName);
        if (!f.renameTo(to)) throw new IllegalStateException("Failed to rename " + f);
        return documentId(to);
    }
}
