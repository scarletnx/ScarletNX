// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.annotation.TargetApi;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** The data folder (src/gui/android.c: .scarletnx in the app's own files) in the system's Files app and its pickers,
    which show nothing of an app's own storage otherwise. A document's ID is its path in the folder after "data/"
    ("data" the folder itself): the folder's own name starts with a dot, and the Files app hides what is under one.
    Nothing outside the folder is reached, through ".." or a link */
public class DataProvider extends DocumentsProvider {
    private static final String ROOT = "data";
    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_ICON,
                                                  Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID,
                                                  Root.COLUMN_AVAILABLE_BYTES};
    private static final String[] DOCUMENT_COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE,
                                                      Document.COLUMN_DISPLAY_NAME, Document.COLUMN_LAST_MODIFIED,
                                                      Document.COLUMN_FLAGS, Document.COLUMN_SIZE};

    /** the provider's authority, as the manifest names it */
    static String authority(android.content.Context context) {
        return context.getPackageName() + ".data";
    }

    /** the data folder, made when missing */
    static File folder(android.content.Context context) {
        File folder = new File(context.getFilesDir(), ".scarletnx");
        folder.mkdirs();
        return folder;
    }

    /** the root's URI, which the Files app opens on */
    static Uri rootUri(android.content.Context context) {
        return DocumentsContract.buildRootUri(authority(context), ROOT);
    }

    /** a file of the data folder's URI, for the share sheet; null for one outside it */
    static Uri documentUri(android.content.Context context, File file) {
        try {
            String path = file.getCanonicalPath(), base = folder(context).getCanonicalPath();
            return path.startsWith(base + "/")
                ? DocumentsContract.buildDocumentUri(authority(context), ROOT + path.substring(base.length()))
                : null;
        } catch (IOException e) {
            return null;
        }
    }

    private File base() {
        File folder = folder(getContext());
        try {
            return folder.getCanonicalFile();
        } catch (IOException e) {
            return folder;
        }
    }

    private String idOf(File file) {
        String base = base().getPath(), path = file.getPath();
        return path.equals(base) ? ROOT : ROOT + path.substring(base.length());
    }

    private File fileOf(String id) throws FileNotFoundException {
        try {
            if (!id.equals(ROOT) && !id.startsWith(ROOT + "/")) throw new IOException(id);
            File file = new File(base(), id.substring(ROOT.length())).getCanonicalFile();
            String base = base().getCanonicalPath(), path = file.getPath();
            if (path.equals(base) || path.startsWith(base + "/")) return file;
        } catch (IOException e) {
            // refused below
        }
        throw new FileNotFoundException(id + " is not in the data folder");
    }

    private static String typeOf(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        String type = dot < 0 ? null : MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                                           name.substring(dot + 1).toLowerCase(java.util.Locale.ROOT));
        return type != null ? type : "application/octet-stream";
    }

    private void row(MatrixCursor cursor, File file) {
        int flags = file.isDirectory() ? Document.FLAG_DIR_SUPPORTS_CREATE : Document.FLAG_SUPPORTS_WRITE;
        if (!file.equals(base())) flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        cursor.newRow()
            .add(Document.COLUMN_DOCUMENT_ID, idOf(file))
            .add(Document.COLUMN_MIME_TYPE, typeOf(file))
            .add(Document.COLUMN_DISPLAY_NAME, file.equals(base()) ? "ScarletNX" : file.getName())
            .add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
            .add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, file.isDirectory() ? null : file.length());
    }

    private void changed(File folder) {
        Uri children = DocumentsContract.buildChildDocumentsUri(authority(getContext()), idOf(folder));
        getContext().getContentResolver().notifyChange(children, null, false);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        File base = base();
        cursor.newRow()
            .add(Root.COLUMN_ROOT_ID, ROOT)
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_LOCAL_ONLY)
            .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
            .add(Root.COLUMN_TITLE, "ScarletNX")
            .add(Root.COLUMN_DOCUMENT_ID, ROOT)
            .add(Root.COLUMN_AVAILABLE_BYTES, base.getFreeSpace());
        return cursor;
    }

    @Override
    public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File file = fileOf(id);
        if (!file.exists()) throw new FileNotFoundException(id);
        row(cursor, file);
        return cursor;
    }

    @Override
    public Cursor queryChildDocuments(String parent, String[] projection, String sortOrder)
        throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File[] files = fileOf(parent).listFiles();
        if (files != null)
            for (File file : files) row(cursor, file);
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal)
        throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileOf(id), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public boolean isChildDocument(String parent, String id) {
        try {
            return fileOf(id).getPath().startsWith(fileOf(parent).getPath() + "/");
        } catch (FileNotFoundException e) {
            return false;
        }
    }

    /** the IDs of each folder from parent (the root when null) down to id: the Files app opens a folder of the data
        there (MainActivity.openFolder), and on its own default place, Downloads, without them */
    @TargetApi(26)
    @Override
    public DocumentsContract.Path findDocumentPath(String parent, String id) throws FileNotFoundException {
        String from = idOf(fileOf(parent != null ? parent : ROOT)), to = idOf(fileOf(id));
        if (!to.equals(from) && !to.startsWith(from + "/")) throw new FileNotFoundException(id + " is not in " + from);
        List<String> ids = new ArrayList<>();
        ids.add(from);
        for (int at = from.length(); at < to.length();) {
            int next = to.indexOf('/', at + 1);
            at = next < 0 ? to.length() : next;
            ids.add(to.substring(0, at));
        }
        return new DocumentsContract.Path(parent != null ? null : ROOT, ids);
    }

    @Override
    public String createDocument(String parent, String type, String name) throws FileNotFoundException {
        File folder = fileOf(parent), file = new File(folder, name);
        for (int n = 1; file.exists(); ++n) file = new File(folder, name + " (" + n + ")");
        boolean made;
        try {
            made = Document.MIME_TYPE_DIR.equals(type) ? file.mkdir() : file.createNewFile();
        } catch (IOException e) {
            made = false;
        }
        if (!made) throw new FileNotFoundException("cannot make " + file.getPath());
        changed(folder);
        return idOf(file);
    }

    @Override
    public void deleteDocument(String id) throws FileNotFoundException {
        File file = fileOf(id);
        if (file.equals(base()) || !delete(file)) throw new FileNotFoundException("cannot delete " + id);
        changed(file.getParentFile());
    }

    private static boolean delete(File file) {
        File[] files = file.isDirectory() && !isLink(file) ? file.listFiles() : null;
        if (files != null)
            for (File child : files) delete(child);
        return file.delete();
    }

    private static boolean isLink(File file) {
        try {
            return !file.getCanonicalFile().equals(file.getAbsoluteFile());
        } catch (IOException e) {
            return true;
        }
    }

    @Override
    public String renameDocument(String id, String name) throws FileNotFoundException {
        File file = fileOf(id), renamed = new File(file.getParentFile(), name);
        if (file.equals(base()) || name.contains("/") || renamed.exists() || !file.renameTo(renamed))
            throw new FileNotFoundException("cannot rename " + id + " to " + name);
        changed(file.getParentFile());
        return idOf(renamed);
    }
}
